#pragma once

#include <memory>
#include <string>

namespace ke {

class IRenderBackend;

// Implemented by each backend translation unit. Returns nullptr and sets
// `error` when the backend is not compiled into this build.
std::unique_ptr<IRenderBackend> createVulkanBackend(std::string& error);
std::unique_ptr<IRenderBackend> createOpenGlBackend(std::string& error);
std::unique_ptr<IRenderBackend> createNullBackend(std::string& error);

} // namespace ke
