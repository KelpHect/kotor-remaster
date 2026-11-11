#include "render/Backend.h"
#include "render/BackendFactory.h"

#include <utility>

namespace ke {

std::unique_ptr<IRenderBackend> createBackend(BackendKind kind, std::string& error)
{
    switch (kind) {
    case BackendKind::Vulkan:
        return createVulkanBackend(error);
    case BackendKind::OpenGL:
        return createOpenGlBackend(error);
    case BackendKind::Null:
        return createNullBackend(error);
    case BackendKind::Auto:
        break;
    }
    error = "Auto backend must be resolved by the application";
    return nullptr;
}

bool setGlWindowAttributes(std::string& error)
{
    if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4)
        || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5)
        || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE)
        || !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)
        || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)
        || !SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8)) {
        error = SDL_GetError();
        return false;
    }
    return true;
}

} // namespace ke
