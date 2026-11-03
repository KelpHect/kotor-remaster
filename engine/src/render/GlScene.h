#pragma once

#include "kotor/Formats.h"
#include "render/SceneView.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ke {

#if defined(_WIN32)
#define KE_GLAPI __stdcall
#else
#define KE_GLAPI
#endif

using GlUint = unsigned int;
using GlInt = int;
using GlEnum = unsigned int;
using GlSizei = int;
using GlFloat = float;
using GlBoolean = unsigned char;
using GlSizeiptr = std::ptrdiff_t;

struct GlApi {
    void(KE_GLAPI* GenVertexArrays)(GlSizei, GlUint*) = nullptr;
    void(KE_GLAPI* BindVertexArray)(GlUint) = nullptr;
    void(KE_GLAPI* DeleteVertexArrays)(GlSizei, const GlUint*) = nullptr;
    void(KE_GLAPI* GenBuffers)(GlSizei, GlUint*) = nullptr;
    void(KE_GLAPI* BindBuffer)(GlEnum, GlUint) = nullptr;
    void(KE_GLAPI* BufferData)(GlEnum, GlSizeiptr, const void*, GlEnum) = nullptr;
    void(KE_GLAPI* DeleteBuffers)(GlSizei, const GlUint*) = nullptr;
    void(KE_GLAPI* EnableVertexAttribArray)(GlUint) = nullptr;
    void(KE_GLAPI* VertexAttribPointer)(GlUint, GlInt, GlEnum, GlBoolean, GlSizei, const void*) = nullptr;
    GlUint(KE_GLAPI* CreateShader)(GlEnum) = nullptr;
    void(KE_GLAPI* ShaderSource)(GlUint, GlSizei, const char* const*, const GlInt*) = nullptr;
    void(KE_GLAPI* CompileShader)(GlUint) = nullptr;
    void(KE_GLAPI* GetShaderiv)(GlUint, GlEnum, GlInt*) = nullptr;
    void(KE_GLAPI* GetShaderInfoLog)(GlUint, GlSizei, GlSizei*, char*) = nullptr;
    GlUint(KE_GLAPI* CreateProgram)() = nullptr;
    void(KE_GLAPI* AttachShader)(GlUint, GlUint) = nullptr;
    void(KE_GLAPI* LinkProgram)(GlUint) = nullptr;
    void(KE_GLAPI* GetProgramiv)(GlUint, GlEnum, GlInt*) = nullptr;
    void(KE_GLAPI* GetProgramInfoLog)(GlUint, GlSizei, GlSizei*, char*) = nullptr;
    void(KE_GLAPI* UseProgram)(GlUint) = nullptr;
    void(KE_GLAPI* DeleteShader)(GlUint) = nullptr;
    void(KE_GLAPI* DeleteProgram)(GlUint) = nullptr;
    GlInt(KE_GLAPI* GetUniformLocation)(GlUint, const char*) = nullptr;
    void(KE_GLAPI* UniformMatrix4fv)(GlInt, GlSizei, GlBoolean, const GlFloat*) = nullptr;
    void(KE_GLAPI* Uniform3f)(GlInt, GlFloat, GlFloat, GlFloat) = nullptr;
    void(KE_GLAPI* Uniform1f)(GlInt, GlFloat) = nullptr;
    void(KE_GLAPI* Uniform1i)(GlInt, GlInt) = nullptr;
    void(KE_GLAPI* ActiveTexture)(GlEnum) = nullptr;
    void(KE_GLAPI* GenTextures)(GlSizei, GlUint*) = nullptr;
    void(KE_GLAPI* BindTexture)(GlEnum, GlUint) = nullptr;
    void(KE_GLAPI* TexImage2D)(GlEnum, GlInt, GlInt, GlSizei, GlSizei, GlInt, GlEnum, GlEnum, const void*) = nullptr;
    void(KE_GLAPI* TexParameteri)(GlEnum, GlEnum, GlInt) = nullptr;
    void(KE_GLAPI* DeleteTextures)(GlSizei, const GlUint*) = nullptr;
    void(KE_GLAPI* DrawElements)(GlEnum, GlSizei, GlEnum, const void*) = nullptr;
    void(KE_GLAPI* Enable)(GlEnum) = nullptr;
    void(KE_GLAPI* Disable)(GlEnum) = nullptr;
    void(KE_GLAPI* BlendFunc)(GlEnum, GlEnum) = nullptr;
    void(KE_GLAPI* DepthFunc)(GlEnum) = nullptr;
    void(KE_GLAPI* ClearDepth)(double) = nullptr;
    void(KE_GLAPI* ClearColor)(GlFloat, GlFloat, GlFloat, GlFloat) = nullptr;
    void(KE_GLAPI* Clear)(GlUint) = nullptr;
    void(KE_GLAPI* Viewport)(GlInt, GlInt, GlSizei, GlSizei) = nullptr;
    void(KE_GLAPI* ReadBuffer)(GlEnum) = nullptr;
    void(KE_GLAPI* ReadPixels)(GlInt, GlInt, GlSizei, GlSizei, GlEnum, GlEnum, void*) = nullptr;
    void(KE_GLAPI* PixelStorei)(GlEnum, GlInt) = nullptr;
    void(KE_GLAPI* GenerateMipmap)(GlEnum) = nullptr;
    GlEnum(KE_GLAPI* GetError)() = nullptr;
};

class GlScene {
public:
    bool initialize(std::string& error);
    void shutdown();
    bool upload(const kotor::Scene& scene, std::string& error);
    void refreshEntityMeshes(const kotor::Scene& scene);
    void render(const SceneCamera& camera, int width, int height);
    bool screenshot(const std::filesystem::path& path, int width, int height);

private:
    struct GlMesh {
        GlUint vao = 0;
        GlUint vbo = 0;
        GlUint ibo = 0;
        std::uint32_t entity = UINT32_MAX;
        std::vector<kotor::TextureBatch> batches;
    };

    GlUint compileShader(GlEnum type, const char* source, std::string& error);
    GlMesh uploadMesh(const std::vector<kotor::SceneVertex>& vertices,
                      const std::vector<std::uint32_t>& indices,
                      const std::vector<kotor::TextureBatch>& batches, std::uint32_t entity);
    void drawMesh(const GlMesh& mesh);
    void destroyGpu();

    GlApi gl_;
    GlUint program_ = 0;
    std::vector<GlMesh> meshes_;
    std::vector<GlUint> textures_;
    const kotor::Scene* scene_ = nullptr;
    kotor::Vec3 ambient_{0.35f, 0.35f, 0.38f};
    kotor::Vec3 diffuse_{0.85f, 0.82f, 0.75f};
    kotor::Vec3 fogColor_{0.06f, 0.07f, 0.10f};
    float fogNear_ = 40.0f;
    float fogFar_ = 160.0f;
    bool loggedFirstFrame_ = false;
};

} // namespace ke
