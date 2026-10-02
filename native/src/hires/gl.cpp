#include "hires/gl.h"

#include <SDL3/SDL.h>

namespace f19::gl {

#define F19_GL_DEFINE(type, name) type name = nullptr;
F19_GL_FUNCS(F19_GL_DEFINE)
#undef F19_GL_DEFINE

bool load() {
    bool ok = true;
#define F19_GL_LOAD(type, name)                                                       \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));                 \
    if (!name) {                                                                      \
        SDL_Log("missing GL function gl%s", #name);                                   \
        ok = false;                                                                   \
    }
    F19_GL_FUNCS(F19_GL_LOAD)
#undef F19_GL_LOAD
    return ok;
}

}  // namespace f19::gl
