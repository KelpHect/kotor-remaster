#pragma once

#include "kotor/Formats.h"
#include "render/Backend.h"
#include "render/SceneView.h"
#include "render/VideoEffect.h"

#include <cstddef>
#include <array>
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
    void(KE_GLAPI* BufferSubData)(GlEnum, GlSizeiptr, GlSizeiptr, const void*) = nullptr;
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
    void(KE_GLAPI* DrawArrays)(GlEnum, GlInt, GlSizei) = nullptr;
    void(KE_GLAPI* CopyTexSubImage2D)(GlEnum, GlInt, GlInt, GlInt, GlInt, GlInt, GlSizei, GlSizei) = nullptr;
    void(KE_GLAPI* Uniform3fv)(GlInt, GlSizei, const GlFloat*) = nullptr;
    void(KE_GLAPI* Scissor)(GlInt, GlInt, GlSizei, GlSizei) = nullptr;
    void(KE_GLAPI* CullFace)(GlEnum) = nullptr;
    void(KE_GLAPI* DepthMask)(GlBoolean) = nullptr;
    void(KE_GLAPI* Uniform4f)(GlInt, GlFloat, GlFloat, GlFloat, GlFloat) = nullptr;
    void(KE_GLAPI* ColorMask)(GlBoolean, GlBoolean, GlBoolean, GlBoolean) = nullptr;
    void(KE_GLAPI* StencilFunc)(GlEnum, GlInt, GlUint) = nullptr;
    void(KE_GLAPI* StencilOp)(GlEnum, GlEnum, GlEnum) = nullptr;
    void(KE_GLAPI* StencilMask)(GlUint) = nullptr;
    void(KE_GLAPI* ClearStencil)(GlInt) = nullptr;
    void(KE_GLAPI* FrontFace)(GlEnum) = nullptr;
};

class GlScene {
public:
    bool initialize(std::string& error);
    void shutdown();
    bool upload(const kotor::Scene& scene, std::string& error);
    // Uploads only the textures and entity meshes it has not seen and drops those that are gone.
    bool syncMeshes(const kotor::Scene& scene, std::string& error);
    void refreshEntityMeshes(const kotor::Scene& scene);
    // Re-uploads vertices [first, first + count) of the area mesh (the grass wind moves them).
    void refreshSceneVertices(const kotor::Scene& scene, std::size_t first, std::size_t count);
    void render(const SceneCamera& camera, int width, int height);
    void setVideoEffect(const VideoEffectState& effect) { effect_ = effect; }
    bool screenshot(const std::filesystem::path& path, int width, int height);
    bool uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                          GlUint& texture, std::string& error);
    void destroyMovieFrame(GlUint& texture);

    // 3D GUI preview: draws one PreviewView of the uploaded (preview) scene into its
    // window rectangle (top-left pixel origin) without touching pixels outside it.
    // Restores viewport and scissor state afterwards. Never clears the whole window.
    void renderView(const PreviewView& view, int windowWidth, int windowHeight);
    // Frees the uploaded scene's GPU resources but keeps the shader program.
    void clearScene() { destroyGpu(); }
    // Reads the window framebuffer (GL_BACK before the swap, GL_FRONT after) into
    // tightly packed RGBA8 with a top-left origin and opaque alpha.
    bool readFramePixels(bool backBuffer, int width, int height,
                         std::vector<std::uint8_t>& rgbaTopLeft);
    // IRenderBackend::requestShadowProbe / shadowProbe.
    void requestShadowProbe() { shadowProbeRequested_ = true; shadowProbeReady_ = false; }
    bool shadowProbe(std::size_t& covered, std::size_t& total) const
    {
        covered = shadowProbeCovered_;
        total = shadowProbeTotal_;
        return shadowProbeReady_;
    }

private:
    struct GlMesh {
        GlUint vao = 0;
        GlUint vbo = 0;
        GlUint ibo = 0;
        std::uint32_t entity = UINT32_MAX;
        bool visible = true;
        bool fx = false; // an effect model: the distortion pass never takes it
        std::vector<kotor::TextureBatch> batches;
        std::uint64_t uid = 0;
        std::size_t vertexCount = 0;
        std::size_t indexCount = 0;
    };

    void uploadTextures(const kotor::Scene& scene, std::size_t first);
    void refreshProceduralTextures();
    void deleteMesh(GlMesh& mesh);

    GlUint compileShader(GlEnum type, const char* source, std::string& error);
    GlMesh uploadMesh(const std::vector<kotor::SceneVertex>& vertices,
                      const std::vector<std::uint32_t>& indices,
                      const std::vector<kotor::TextureBatch>& batches, std::uint32_t entity);
    void bindExtraAttributes();
    void drawBatch(const GlMesh* mesh, const kotor::TextureBatch& batch, const kotor::SampledLight& light);
    std::size_t drawMeshes(const SceneCamera& camera, float aspect, const PreviewView* view);
    void destroyGpu();
    bool createPostProgram(std::string& error);
    void applyVideoEffect(int width, int height);
    void ensureSceneCopy(int width, int height);
    void applySpeedBlur(int width, int height);
    void applyDistortion(const SceneCamera& camera, int width, int height);
    bool createShadowProgram(std::string& error);
    // Scene::RenderShadows 00451a50, between the static geometry and the rest.
    void drawShadows(const SceneCamera& camera, const float* viewProjection);

    GlApi gl_;
    GlUint shadowProgram_ = 0;
    GlUint shadowVao_ = 0;
    GlUint shadowVbo_ = 0;
    GlInt shadowViewProjection_ = -1;
    GlInt shadowFog_ = -1;
    GlInt shadowEye_ = -1;
    GlInt shadowFogFar_ = -1;
    GlUint program_ = 0;
    struct {
        GlInt model = -1, viewProjection = -1, ambient = -1;
        std::array<GlInt, 3> lightPosition{-1, -1, -1};
        std::array<GlInt, 3> lightColour{-1, -1, -1};
        GlInt fogColor = -1, eye = -1, fogNear = -1, fogFar = -1, fogOn = -1, uv = -1, params = -1;
        GlInt texture = -1, lightmap = -1, envMap = -1, bumpMap = -1, bump = -1, bumpSpecular = -1;
    } uniforms_;
    GlUint particleVao_ = 0;
    GlUint particleVbo_ = 0;
    bool fogOn_ = false;
    kotor::Vec3 frameEye_{};
    bool fogForFrame_ = false; // fog for this pass; a batch can still turn it off for its draw
    std::vector<GlMesh> meshes_;
    std::vector<GlUint> textures_;
    std::vector<std::uint32_t> textureRevisions_;
    const kotor::Scene* scene_ = nullptr;
    kotor::Vec3 ambient_{0.35f, 0.35f, 0.38f};
    kotor::Vec3 diffuse_{0.85f, 0.82f, 0.75f};
    kotor::Vec3 fogColor_{0.06f, 0.07f, 0.10f};
    float fogNear_ = 40.0f;
    float fogFar_ = 160.0f;
    bool loggedFirstFrame_ = false;
    bool shadowProbeRequested_ = false;
    bool shadowProbeReady_ = false;
    std::size_t shadowProbeCovered_ = 0;
    std::size_t shadowProbeTotal_ = 0;

    // Video effect post pass (ACT_508/510): the scene is copied to a texture
    // and redrawn through the effect program, as retail does.
    VideoEffectState effect_;
    GlUint postProgram_ = 0;
    GlUint postVao_ = 0;
    GlUint sceneCopy_ = 0;
    GlUint noiseTexture_ = 0;
    int copyWidth_ = 0;
    int copyHeight_ = 0;
    // Speed blur: the last blurred frame (retail's pbuffer texture 007a68c0) and whether it holds one
    // (007a698d).
    GlUint blurAccum_ = 0;
    int blurWidth_ = 0;
    int blurHeight_ = 0;
    bool blurPrimed_ = false;
};

} // namespace ke
