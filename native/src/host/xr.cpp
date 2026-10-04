#include "host/xr.h"

#include "hires/gl.h"

#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace f19 {

using namespace gl;

namespace {

constexpr int64_t kGlSrgb8Alpha8 = 0x8C43, kGlRgba8 = 0x8058;
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
    std::vector<GLuint> fbos;  // one per swapchain image
    uint32_t index = 0;
};

}  // namespace

struct XrOutput::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    bool running = false, lost = false;
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

    bool make_swapchain(Swapchain& sc, int64_t format, int w, int h);
    bool make_play_space(const XrPosef& pose);
    void try_recenter(XrTime t);
    GLuint acquire(Swapchain& sc);
};

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
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
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
    sc.w = w;
    sc.h = h;
    return true;
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

GLuint XrOutput::Impl::acquire(Swapchain& sc) {
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    check(instance, xrAcquireSwapchainImage(sc.handle, &ai, &sc.index), "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    check(instance, xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
    return sc.fbos[sc.index];
}

XrOutput::XrOutput() : p_(std::make_unique<Impl>()) {}

XrOutput::~XrOutput() {
    auto& d = *p_;
    for (Swapchain* sc : {&d.eyes[0], &d.eyes[1], &d.screen})
        if (sc->handle) xrDestroySwapchain(sc->handle);
    for (XrSpace s : {d.play_space, d.base_space, d.view_space})
        if (s) xrDestroySpace(s);
    if (d.session) xrDestroySession(d.session);
    if (d.instance) xrDestroyInstance(d.instance);
}

bool XrOutput::init(float resolution_scale) {
    auto& d = *p_;
    const char* exts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "F-19 Stealth Fighter");
    ici.applicationInfo.applicationVersion = 1;
    std::strcpy(ici.applicationInfo.engineName, "f19native");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    if (!check(nullptr, xrCreateInstance(&ici, &d.instance), "xrCreateInstance (is an OpenXR runtime installed and active?)"))
        return false;
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(d.instance, xrGetSystem(d.instance, &sgi, &d.system), "xrGetSystem (headset connected? monado-service running?)"))
        return false;
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(d.instance, d.system, &props);

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
    for (int64_t want : {kGlSrgb8Alpha8, kGlRgba8})
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
                 format == kGlSrgb8Alpha8 ? "sRGB" : "RGBA8");
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

void XrOutput::release_eye(int i) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(p_->eyes[i].handle, &ri);
}

GLuint XrOutput::acquire_screen() {
    p_->drew_screen = true;
    return p_->acquire(p_->screen);
}

void XrOutput::release_screen() {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(p_->screen.handle, &ri);
}

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

void XrOutput::recenter() { p_->recenter_pending = true; }

}  // namespace f19
