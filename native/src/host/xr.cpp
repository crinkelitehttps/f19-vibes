#include "host/xr.h"

#include "hires/gl.h"
#include "host/gamepad.h"

#ifdef _WIN32
// Windows: Direct3D 11 swapchains, which every Windows runtime supports (the
// WMR runtime has no OpenGL). Each eye is rendered with GL into a GL
// framebuffer, blitted (flipped: D3D is top-down) into a D3D11 texture
// shared with GL through WGL_NV_DX_interop2, then copied into the runtime's
// swapchain image.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#else
#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

namespace f19 {

using namespace gl;

namespace {

#ifdef _WIN32
constexpr int64_t kFormatSrgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kFormatRgba = DXGI_FORMAT_R8G8B8A8_UNORM;

// WGL_NV_DX_interop2 (wglext.h).
constexpr GLenum kWglAccessWriteDiscardNV = 0x0002;
using PFNWGLDXOPENDEVICENVPROC = HANDLE(WINAPI*)(void* dx_device);
using PFNWGLDXCLOSEDEVICENVPROC = BOOL(WINAPI*)(HANDLE device);
using PFNWGLDXREGISTEROBJECTNVPROC = HANDLE(WINAPI*)(HANDLE device, void* dx_object, GLuint name, GLenum type, GLenum access);
using PFNWGLDXUNREGISTEROBJECTNVPROC = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using PFNWGLDXLOCKOBJECTSNVPROC = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);
using PFNWGLDXUNLOCKOBJECTSNVPROC = PFNWGLDXLOCKOBJECTSNVPROC;
struct WglInterop {
    PFNWGLDXOPENDEVICENVPROC OpenDevice = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC CloseDevice = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC RegisterObject = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC UnregisterObject = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC LockObjects = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC UnlockObjects = nullptr;
    bool load() {
        auto get = [](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(wglGetProcAddress(name)));
            return fn != nullptr;
        };
        return get(OpenDevice, "wglDXOpenDeviceNV") && get(CloseDevice, "wglDXCloseDeviceNV") &&
               get(RegisterObject, "wglDXRegisterObjectNV") && get(UnregisterObject, "wglDXUnregisterObjectNV") &&
               get(LockObjects, "wglDXLockObjectsNV") && get(UnlockObjects, "wglDXUnlockObjectsNV");
    }
};

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
#else
constexpr int64_t kFormatSrgb = 0x8C43 /* GL_SRGB8_ALPHA8 */, kFormatRgba = 0x8058 /* GL_RGBA8 */;
#endif
// Virtual screen: 4:3, this wide (m), this far ahead of the neutral eye point.
constexpr float kScreenWidthM = 1.6f, kScreenDistM = 1.8f;
// Head positions farther than this from the neutral eye point are tracking failures.
constexpr float kMaxHeadOffsetM = 1.0f;

bool check(XrInstance inst, XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char s[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (inst) xrResultToString(inst, r, s);
    std::fprintf(stderr, "openxr: %s failed: %s (%d)\n", what, s, int(r));
    return false;
}

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int w = 0, h = 0;
    uint32_t index = 0;
#ifdef _WIN32
    std::vector<ID3D11Texture2D*> images;  // the runtime's
    GLuint render_fbo = 0, render_tex = 0;  // what GL renders into
    ID3D11Texture2D* shared = nullptr;      // GL's view of it: interop_tex
    GLuint interop_tex = 0, interop_fbo = 0;
    HANDLE interop = nullptr;
#else
    std::vector<GLuint> fbos;  // one per swapchain image
#endif
};

}  // namespace

struct XrOutput::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    bool running = false, lost = false, focused = false;
    // base: the runtime's LOCAL space; play: LOCAL moved to the recentred
    // eye point (yaw only), which all rendering uses.
    XrSpace base_space = XR_NULL_HANDLE, play_space = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;
    bool recenter_pending = true;
    Swapchain eyes[2], screen;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    bool in_frame = false, views_valid = false, drew_eyes = false, drew_screen = false;
    bool position_rejected = false;
    bool log = std::getenv("F19_XR_LOG") != nullptr;  // print the eye pose twice a second
    XrTime last_log = 0;
    // Motion controller actions; each has both hands as subaction paths.
    XrActionSet action_set = XR_NULL_HANDLE;
    XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrAction trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, stick = XR_NULL_HANDLE,
             stick_click = XR_NULL_HANDLE, pad = XR_NULL_HANDLE, pad_click = XR_NULL_HANDLE, pad_touch = XR_NULL_HANDLE,
             upper = XR_NULL_HANDLE, lower = XR_NULL_HANDLE;  // face buttons (B/Y, A/X): trackpad up / down
    bool hp_controller = false;  // XR_EXT_hp_mixed_reality_controller enabled
    bool controllers_announced[2] = {};

#ifdef _WIN32
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    WglInterop wgl;
    HANDLE interop_device = nullptr;
    bool make_device();
#endif

    bool make_swapchain(Swapchain& sc, int64_t format, int w, int h);
    bool make_play_space(const XrPosef& pose);
    void try_recenter(XrTime t);
    GLuint acquire(Swapchain& sc);
    void release(Swapchain& sc);
    bool make_actions();
};

#ifdef _WIN32
// The D3D11 device on the adapter the runtime drives the headset with, and
// GL's access to it.
bool XrOutput::Impl::make_device() {
    PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!get_reqs || !check(instance, get_reqs(instance, system, &reqs), "xrGetD3D11GraphicsRequirementsKHR")) return false;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        std::fprintf(stderr, "openxr: CreateDXGIFactory1 failed\n");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !std::memcmp(&desc.AdapterLuid, &reqs.adapterLuid, sizeof(LUID))) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
            std::fprintf(stderr, "openxr: headset GPU: %s\n", name);
            break;
        }
        release(adapter);
    }
    release(factory);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                   UINT(std::size(levels)), D3D11_SDK_VERSION, &device, &got, &context);
    release(adapter);
    if (FAILED(hr) || got < reqs.minFeatureLevel) {
        std::fprintf(stderr, "openxr: cannot create a Direct3D 11 device (0x%08lx)\n", (unsigned long)hr);
        return false;
    }
    if (!wgl.load()) {
        std::fprintf(stderr, "openxr: the OpenGL driver lacks WGL_NV_DX_interop2 (needed to render to Direct3D)\n");
        return false;
    }
    interop_device = wgl.OpenDevice(device);
    if (!interop_device) {
        std::fprintf(stderr,
                     "openxr: wglDXOpenDeviceNV failed: OpenGL is probably running on a different GPU from the headset "
                     "(set f19 to the high-performance GPU in Windows graphics settings)\n");
        return false;
    }
    return true;
}
#endif

bool XrOutput::Impl::make_actions() {
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asi.actionSetName, "flight");
    std::strcpy(asi.localizedActionSetName, "Flight");
    if (!check(instance, xrCreateActionSet(instance, &asi, &action_set), "xrCreateActionSet")) return false;
    xrStringToPath(instance, "/user/hand/left", &hands[0]);
    xrStringToPath(instance, "/user/hand/right", &hands[1]);
    auto make = [&](XrAction& a, const char* name, const char* label, XrActionType type) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ai.actionName, name);
        std::strcpy(ai.localizedActionName, label);
        ai.actionType = type;
        ai.countSubactionPaths = 2;
        ai.subactionPaths = hands;
        return check(instance, xrCreateAction(action_set, &ai, &a), "xrCreateAction");
    };
    if (!make(trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make(squeeze, "squeeze", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(stick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
        !make(stick_click, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(pad, "trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
        !make(pad_click, "trackpad_click", "Trackpad click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(pad_touch, "trackpad_touch", "Trackpad touch", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(upper, "upper_button", "Upper face button", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !make(lower, "lower_button", "Lower face button", XR_ACTION_TYPE_BOOLEAN_INPUT))
        return false;

    // The bindings are written for the WMR controllers; other controllers
    // get what matches. Inputs: "both/..." on either hand, "left/..." or
    // "right/..." on one. Floats bound to buttons are thresholded by the
    // runtime.
    struct Input { XrAction action; const char* input; };
    struct Profile { const char* path; bool available; std::vector<Input> inputs; };
    const Profile profiles[] = {
        {"microsoft/motion_controller", true,
         {{trigger, "both/trigger/value"}, {squeeze, "both/squeeze/click"}, {menu, "both/menu/click"},
          {stick, "both/thumbstick"}, {stick_click, "both/thumbstick/click"}, {pad, "both/trackpad"},
          {pad_click, "both/trackpad/click"}, {pad_touch, "both/trackpad/touch"}}},
        {"hp/mixed_reality_controller", hp_controller,  // Reverb G2
         {{trigger, "both/trigger/value"}, {squeeze, "both/squeeze/value"}, {menu, "both/menu/click"},
          {stick, "both/thumbstick"}, {stick_click, "both/thumbstick/click"},
          {upper, "left/y/click"}, {lower, "left/x/click"}, {upper, "right/b/click"}, {lower, "right/a/click"}}},
        {"oculus/touch_controller", true,  // no right menu (the system button)
         {{trigger, "both/trigger/value"}, {squeeze, "both/squeeze/value"}, {menu, "left/menu/click"},
          {stick, "both/thumbstick"}, {stick_click, "both/thumbstick/click"},
          {upper, "left/y/click"}, {lower, "left/x/click"}, {upper, "right/b/click"}, {lower, "right/a/click"}}},
        {"valve/index_controller", true,  // no menu: B is the menu button
         {{trigger, "both/trigger/value"}, {squeeze, "both/squeeze/value"}, {menu, "both/b/click"},
          {stick, "both/thumbstick"}, {stick_click, "both/thumbstick/click"}, {pad, "both/trackpad"},
          {pad_click, "both/trackpad/force"}, {pad_touch, "both/trackpad/touch"}, {lower, "both/a/click"}}},
        {"htc/vive_controller", true,  // no thumbstick
         {{trigger, "both/trigger/value"}, {squeeze, "both/squeeze/click"}, {menu, "both/menu/click"},
          {pad, "both/trackpad"}, {pad_click, "both/trackpad/click"}, {pad_touch, "both/trackpad/touch"}}},
    };
    int suggested = 0;
    for (const Profile& prof : profiles) {
        if (!prof.available) continue;
        std::vector<XrActionSuggestedBinding> sb;
        for (const Input& in : prof.inputs) {
            std::string input = in.input;
            const bool both = input.rfind("both/", 0) == 0;
            for (const char* hand : {"left", "right"}) {
                if (!both && input.rfind(hand, 0) != 0) continue;
                std::string path = std::string("/user/hand/") + hand + "/input/" + input.substr(input.find('/') + 1);
                XrPath xp;
                xrStringToPath(instance, path.c_str(), &xp);
                sb.push_back({in.action, xp});
            }
        }
        XrInteractionProfileSuggestedBinding isb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        xrStringToPath(instance, (std::string("/interaction_profiles/") + prof.path).c_str(), &isb.interactionProfile);
        isb.countSuggestedBindings = uint32_t(sb.size());
        isb.suggestedBindings = sb.data();
        if (check(instance, xrSuggestInteractionProfileBindings(instance, &isb), prof.path)) suggested++;
    }
    if (!suggested) return false;

    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &action_set;
    return check(instance, xrAttachSessionActionSets(session, &at), "xrAttachSessionActionSets");
}

bool XrOutput::Impl::make_swapchain(Swapchain& sc, int64_t format, int w, int h) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = uint32_t(w);
    ci.height = uint32_t(h);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!check(instance, xrCreateSwapchain(session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
    sc.w = w;
    sc.h = h;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
#ifdef _WIN32
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!check(instance, xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())),
               "xrEnumerateSwapchainImages"))
        return false;
    for (auto& img : imgs) sc.images.push_back(img.texture);
    // Same size, and a format in the swapchain's typeless group, so that
    // CopyResource copies the bytes as they are.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(w);
    td.Height = UINT(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &sc.shared))) {
        std::fprintf(stderr, "openxr: cannot create the shared texture\n");
        return false;
    }
    GenTextures(1, &sc.interop_tex);
    sc.interop = wgl.RegisterObject(interop_device, sc.shared, sc.interop_tex, GL_TEXTURE_2D, kWglAccessWriteDiscardNV);
    if (!sc.interop) {
        std::fprintf(stderr, "openxr: wglDXRegisterObjectNV failed\n");
        return false;
    }
    GenTextures(1, &sc.render_tex);
    BindTexture(GL_TEXTURE_2D, sc.render_tex);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    BindTexture(GL_TEXTURE_2D, 0);
    auto make_fbo = [](GLuint tex, GLuint& fbo) {
        GenFramebuffers(1, &fbo);
        BindFramebuffer(GL_FRAMEBUFFER, fbo);
        FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        return CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    };
    bool ok = make_fbo(sc.render_tex, sc.render_fbo);
    // The interop texture is only usable while locked.
    wgl.LockObjects(interop_device, 1, &sc.interop);
    ok = make_fbo(sc.interop_tex, sc.interop_fbo) && ok;
    wgl.UnlockObjects(interop_device, 1, &sc.interop);
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) std::fprintf(stderr, "openxr: swapchain framebuffer incomplete\n");
    return ok;
#else
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (!check(instance, xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())),
               "xrEnumerateSwapchainImages"))
        return false;
    for (auto& img : imgs) {
        GLuint fbo = 0;
        GenFramebuffers(1, &fbo);
        BindFramebuffer(GL_FRAMEBUFFER, fbo);
        FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, img.image, 0);
        if (CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "openxr: swapchain framebuffer incomplete\n");
            return false;
        }
        sc.fbos.push_back(fbo);
    }
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
#endif
}

bool XrOutput::Impl::make_play_space(const XrPosef& pose) {
    if (play_space) xrDestroySpace(play_space);
    play_space = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace = pose;
    return check(instance, xrCreateReferenceSpace(session, &rs, &play_space), "xrCreateReferenceSpace");
}

// Put the play space's origin at the head, facing where it faces (yaw
// only, so the horizon stays level). Stays pending until the head is
// tracked.
void XrOutput::Impl::try_recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(view_space, base_space, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;
    const XrQuaternionf& q = loc.pose.orientation;
    // Forward (-z) in LOCAL space, projected onto the floor.
    float fx = -2 * (q.x * q.z + q.y * q.w), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float yaw = std::atan2(-fx, -fz);
    XrPosef pose{};
    pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) pose.position = loc.pose.position;
    if (make_play_space(pose)) recenter_pending = false;
}

// GL renders into the swapchain image (Linux) or our own framebuffer, the
// image being acquired at release (Windows).
GLuint XrOutput::Impl::acquire(Swapchain& sc) {
#ifdef _WIN32
    return sc.render_fbo;
#else
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    check(instance, xrAcquireSwapchainImage(sc.handle, &ai, &sc.index), "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    check(instance, xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
    return sc.fbos[sc.index];
#endif
}

void XrOutput::Impl::release(Swapchain& sc) {
#ifdef _WIN32
    // GL: flip into the shared texture (unlocking hands it to D3D).
    wgl.LockObjects(interop_device, 1, &sc.interop);
    Disable(GL_SCISSOR_TEST);
    BindFramebuffer(GL_READ_FRAMEBUFFER, sc.render_fbo);
    BindFramebuffer(GL_DRAW_FRAMEBUFFER, sc.interop_fbo);
    BlitFramebuffer(0, 0, sc.w, sc.h, 0, sc.h, sc.w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    BindFramebuffer(GL_FRAMEBUFFER, 0);
    wgl.UnlockObjects(interop_device, 1, &sc.interop);
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(instance, xrAcquireSwapchainImage(sc.handle, &ai, &sc.index), "xrAcquireSwapchainImage")) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    check(instance, xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
    context->CopyResource(sc.images[sc.index], sc.shared);
#endif
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
}

XrOutput::XrOutput() : p_(std::make_unique<Impl>()) {}

XrOutput::~XrOutput() {
    auto& d = *p_;
    for (Swapchain* sc : {&d.eyes[0], &d.eyes[1], &d.screen}) {
#ifdef _WIN32
        // (The GL context may be gone already: SDL is shut down first.)
        if (sc->interop && wglGetCurrentContext()) d.wgl.UnregisterObject(d.interop_device, sc->interop);
        release(sc->shared);
#endif
        if (sc->handle) xrDestroySwapchain(sc->handle);
    }
    for (XrSpace s : {d.play_space, d.base_space, d.view_space})
        if (s) xrDestroySpace(s);
    if (d.action_set) xrDestroyActionSet(d.action_set);
    if (d.session) xrDestroySession(d.session);
    if (d.instance) xrDestroyInstance(d.instance);
#ifdef _WIN32
    if (d.interop_device && wglGetCurrentContext()) d.wgl.CloseDevice(d.interop_device);
    release(d.context);
    release(d.device);
#endif
}

bool XrOutput::init(float resolution_scale) {
    auto& d = *p_;
#ifdef _WIN32
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
#else
    std::vector<const char*> exts = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
#endif
    {
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (auto& e : props)
            if (!std::strcmp(e.extensionName, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME)) {
                exts.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
                d.hp_controller = true;
            }
    }
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "F-19 Stealth Fighter");
    ici.applicationInfo.applicationVersion = 1;
    std::strcpy(ici.applicationInfo.engineName, "f19native");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.enabledExtensionNames = exts.data();
    if (!check(nullptr, xrCreateInstance(&ici, &d.instance), "xrCreateInstance (is an OpenXR runtime installed and active?)"))
        return false;
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(d.instance, xrGetSystem(d.instance, &sgi, &d.system), "xrGetSystem (headset connected and the VR runtime running?)"))
        return false;
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(d.instance, d.system, &props);

#ifdef _WIN32
    if (!d.make_device()) return false;
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = d.device;
#else
    PFN_xrGetOpenGLGraphicsRequirementsKHR get_reqs = nullptr;
    xrGetInstanceProcAddr(d.instance, "xrGetOpenGLGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));
    XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (!get_reqs || !check(d.instance, get_reqs(d.instance, d.system, &reqs), "xrGetOpenGLGraphicsRequirementsKHR")) return false;

    // The runtime shares our GLX context.
    XrGraphicsBindingOpenGLXlibKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    gb.xDisplay = glXGetCurrentDisplay();
    gb.glxContext = glXGetCurrentContext();
    gb.glxDrawable = glXGetCurrentDrawable();
    if (!gb.xDisplay || !gb.glxContext) {
        std::fprintf(stderr, "openxr: needs a GLX (X11) OpenGL context\n");
        return false;
    }
    int fbconfig_id = 0, screen = DefaultScreen(gb.xDisplay);
    glXQueryContext(gb.xDisplay, gb.glxContext, GLX_FBCONFIG_ID, &fbconfig_id);
    glXQueryContext(gb.xDisplay, gb.glxContext, GLX_SCREEN, &screen);
    int attrs[] = {GLX_FBCONFIG_ID, fbconfig_id, None}, n = 0;
    if (GLXFBConfig* cfgs = glXChooseFBConfig(gb.xDisplay, screen, attrs, &n)) {
        if (n > 0) {
            gb.glxFBConfig = cfgs[0];
            if (XVisualInfo* vi = glXGetVisualFromFBConfig(gb.xDisplay, cfgs[0])) {
                gb.visualid = uint32_t(vi->visualid);
                XFree(vi);
            }
        }
        XFree(cfgs);
    }
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = d.system;
    if (!check(d.instance, xrCreateSession(d.instance, &sci, &d.session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!check(d.instance, xrCreateReferenceSpace(d.session, &rs, &d.base_space), "xrCreateReferenceSpace")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(d.instance, xrCreateReferenceSpace(d.session, &rs, &d.view_space), "xrCreateReferenceSpace")) return false;
    if (!d.make_play_space(rs.poseInReferenceSpace)) return false;
    if (!d.make_actions()) std::fprintf(stderr, "openxr: motion controllers unavailable\n");

    uint32_t nv = 0;
    xrEnumerateViewConfigurationViews(d.instance, d.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
    std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(d.instance, d.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data());
    if (nv != 2) {
        std::fprintf(stderr, "openxr: expected 2 views, got %u\n", nv);
        return false;
    }

    // sRGB swapchain written without GL_FRAMEBUFFER_SRGB: our colours are
    // already sRGB-encoded, and the runtime decodes them correctly.
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(d.session, 0, &nf, nullptr);
    std::vector<int64_t> formats(nf);
    xrEnumerateSwapchainFormats(d.session, nf, &nf, formats.data());
    int64_t format = 0;
    for (int64_t want : {kFormatSrgb, kFormatRgba})
        if (!format && std::find(formats.begin(), formats.end(), want) != formats.end()) format = want;
    if (!format) {
        std::fprintf(stderr, "openxr: no usable swapchain format\n");
        return false;
    }
    for (int i = 0; i < 2; i++) {
        int w = std::clamp(int(vcv[i].recommendedImageRectWidth * resolution_scale), 16, int(vcv[i].maxImageRectWidth));
        int h = std::clamp(int(vcv[i].recommendedImageRectHeight * resolution_scale), 16, int(vcv[i].maxImageRectHeight));
        if (!d.make_swapchain(d.eyes[i], format, w, h)) return false;
    }
    if (!d.make_swapchain(d.screen, format, kScreenW, kScreenH)) return false;
    std::fprintf(stderr, "openxr: %s, %d x %d per eye (recommended %u x %u), %s swapchains\n", props.systemName, d.eyes[0].w,
                 d.eyes[0].h, vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight,
                 format == kFormatSrgb ? "sRGB" : "RGBA8");
    return true;
}

bool XrOutput::poll() {
    auto& d = *p_;
    if (!d.session || d.lost) return false;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(d.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            if (e.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (check(d.instance, xrBeginSession(d.session, &bi), "xrBeginSession")) {
                    d.running = true;
                    d.recenter_pending = true;
                    std::fprintf(stderr, "openxr: session running (Shift+F12 recentres)\n");
                }
            } else if (e.state == XR_SESSION_STATE_FOCUSED || e.state == XR_SESSION_STATE_VISIBLE) {
                d.focused = e.state == XR_SESSION_STATE_FOCUSED;
            } else if (e.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(d.session);
                d.running = false;
                std::fprintf(stderr, "openxr: session stopped\n");
            } else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING) {
                d.running = false;
                d.lost = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            d.running = false;
            d.lost = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !d.lost;
}

bool XrOutput::running() const { return p_->running; }
bool XrOutput::tracking() const { return p_->views_valid; }
int XrOutput::eye_width() const { return p_->eyes[0].w; }
int XrOutput::eye_height() const { return p_->eyes[0].h; }

bool XrOutput::begin_frame() {
    auto& d = *p_;
    d.frame = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (!check(d.instance, xrWaitFrame(d.session, &wi, &d.frame), "xrWaitFrame")) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!check(d.instance, xrBeginFrame(d.session, &bi), "xrBeginFrame")) return false;
    d.in_frame = true;
    d.drew_eyes = d.drew_screen = false;
    if (d.recenter_pending) d.try_recenter(d.frame.predictedDisplayTime);

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = d.frame.predictedDisplayTime;
    li.space = d.play_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    d.views_valid = XR_SUCCEEDED(xrLocateViews(d.session, &li, &vs, 2, &n, d.views)) && n == 2 &&
                    (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    // A failing positional tracker (Basalt losing its fix) can report the
    // head kilometres away. In a seat that is never real: drop the head
    // position but keep the eyes' separation. The submitted poses are
    // changed too, so the compositor reprojects what was rendered.
    if (d.views_valid) {
        XrVector3f c{};
        for (const XrView& v : d.views) {
            c.x += v.pose.position.x / 2;
            c.y += v.pose.position.y / 2;
            c.z += v.pose.position.z / 2;
        }
        float dist = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
        bool bad = !(dist <= kMaxHeadOffsetM);  // also NaN
        if (bad)
            for (XrView& v : d.views) {
                v.pose.position.x -= c.x;
                v.pose.position.y -= c.y;
                v.pose.position.z -= c.z;
                if (!std::isfinite(v.pose.position.x + v.pose.position.y + v.pose.position.z)) v.pose.position = {};
            }
        if (bad != d.position_rejected) {
            if (bad) std::fprintf(stderr, "openxr: head position %.1f m from the seat; ignoring position (tracking lost?)\n", dist);
            else std::fprintf(stderr, "openxr: head position back near the seat\n");
            d.position_rejected = bad;
        }
    }
    if (d.log && d.frame.predictedDisplayTime - d.last_log >= 500000000) {
        d.last_log = d.frame.predictedDisplayTime;
        const XrPosef& p = d.views[0].pose;
        std::fprintf(stderr, "openxr: left eye pos %+.3f %+.3f %+.3f m, quat %+.3f %+.3f %+.3f %+.3f, flags %llx\n", p.position.x,
                     p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w,
                     (unsigned long long)vs.viewStateFlags);
    }
    return d.frame.shouldRender;
}

EyeView XrOutput::eye(int i) const {
    const XrView& v = p_->views[i];
    const XrQuaternionf& q = v.pose.orientation;
    // Rotation matrix of the pose (view -> play space).
    const float R[3][3] = {
        {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
        {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
        {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)},
    };
    // OpenXR looks down -z; the aircraft frame (and our view space) has z
    // forward: flip z on both sides.
    const float s[3] = {1, 1, -1};
    EyeView e;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) e.rot[r][c] = s[r] * s[c] * R[r][c];
    e.pos[0] = v.pose.position.x * 100;
    e.pos[1] = v.pose.position.y * 100;
    e.pos[2] = -v.pose.position.z * 100;
    e.tan_left = std::tan(v.fov.angleLeft);
    e.tan_right = std::tan(v.fov.angleRight);
    e.tan_down = std::tan(v.fov.angleDown);
    e.tan_up = std::tan(v.fov.angleUp);
    return e;
}

GLuint XrOutput::acquire_eye(int i) {
    p_->drew_eyes = true;
    return p_->acquire(p_->eyes[i]);
}

void XrOutput::release_eye(int i) { p_->release(p_->eyes[i]); }

GLuint XrOutput::acquire_screen() {
    p_->drew_screen = true;
    return p_->acquire(p_->screen);
}

void XrOutput::release_screen() { p_->release(p_->screen); }

void XrOutput::end_frame() {
    auto& d = *p_;
    if (!d.in_frame) return;
    d.in_frame = false;
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t count = 0;
    if (d.drew_eyes) {
        for (int i = 0; i < 2; i++) {
            pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[i].pose = d.views[i].pose;
            pv[i].fov = d.views[i].fov;
            pv[i].subImage.swapchain = d.eyes[i].handle;
            pv[i].subImage.imageRect = {{0, 0}, {d.eyes[i].w, d.eyes[i].h}};
        }
        proj.space = d.play_space;
        proj.viewCount = 2;
        proj.views = pv;
        layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    } else if (d.drew_screen) {
        quad.space = d.play_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = d.screen.handle;
        quad.subImage.imageRect = {{0, 0}, {d.screen.w, d.screen.h}};
        quad.pose.orientation.w = 1;
        quad.pose.position = {0, -0.1f, -kScreenDistM};
        quad.size = {kScreenWidthM, kScreenWidthM * 0.75f};
        layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = d.frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = count;
    ei.layers = layers;
    check(d.instance, xrEndFrame(d.session, &ei), "xrEndFrame");
}

bool XrOutput::controllers(MotionControllers& out) {
    auto& d = *p_;
    out = {};
    if (!d.action_set || !d.running || !d.focused) return false;
    XrActiveActionSet active{d.action_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (!XR_SUCCEEDED(xrSyncActions(d.session, &si))) return false;
    for (int h = 0; h < 2; h++) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = d.hands[h];
        auto get_bool = [&](XrAction a, bool& v) {
            gi.action = a;
            XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(d.session, &gi, &st)) && st.isActive) {
                v = st.currentState;
                out.active[h] = true;
            }
        };
        auto get_vec = [&](XrAction a, float* v) {
            gi.action = a;
            XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(d.session, &gi, &st)) && st.isActive) {
                v[0] = st.currentState.x;
                v[1] = -st.currentState.y;  // OpenXR: up +
                out.active[h] = true;
            }
        };
        gi.action = d.trigger;
        XrActionStateFloat tr{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(d.session, &gi, &tr)) && tr.isActive) {
            out.trigger[h] = tr.currentState;
            out.active[h] = true;
        }
        get_bool(d.squeeze, out.squeeze[h]);
        get_bool(d.menu, out.menu[h]);
        get_vec(d.stick, out.stick[h]);
        get_bool(d.stick_click, out.stick_click[h]);
        get_vec(d.pad, out.pad[h]);
        get_bool(d.pad_click, out.pad_click[h]);
        get_bool(d.pad_touch, out.pad_touch[h]);
        // Face buttons: clicks at the top / bottom of the trackpad.
        bool up = false, down = false;
        get_bool(d.upper, up);
        get_bool(d.lower, down);
        if (!out.pad_click[h] && (up || down)) {
            out.pad_click[h] = true;
            out.pad[h][0] = 0;
            out.pad[h][1] = up ? -1.0f : 1.0f;
        }
        if (out.active[h] != d.controllers_announced[h]) {
            std::fprintf(stderr, "openxr: %s controller %s\n", h ? "right" : "left", out.active[h] ? "active" : "inactive");
            d.controllers_announced[h] = out.active[h];
        }
    }
    return out.active[0] || out.active[1];
}

void XrOutput::recenter() { p_->recenter_pending = true; }

}  // namespace f19
