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

} // namespace ke
