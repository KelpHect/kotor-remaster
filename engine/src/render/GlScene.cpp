#include "render/GlScene.h"
#include "render/Bmp.h"

#include "core/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <vector>

namespace ke {

namespace {

constexpr GlEnum kGlArrayBuffer = 0x8892u;
constexpr GlEnum kGlElementArrayBuffer = 0x8893u;
constexpr GlEnum kGlStaticDraw = 0x88E4u;
constexpr GlEnum kGlFloat = 0x1406u;
constexpr GlEnum kGlTriangles = 0x0004u;
constexpr GlEnum kGlUnsignedInt = 0x1405u;
constexpr GlEnum kGlTexture2D = 0x0DE1u;
constexpr GlEnum kGlRgba = 0x1908u;
constexpr GlEnum kGlRgba8 = 0x8058u;
constexpr GlEnum kGlUnsignedByte = 0x1401u;
constexpr GlEnum kGlLinear = 0x2601u;
constexpr GlEnum kGlTextureMinFilter = 0x2801u;
constexpr GlEnum kGlTextureMagFilter = 0x2800u;
constexpr GlEnum kGlTextureWrapS = 0x2802u;
constexpr GlEnum kGlTextureWrapT = 0x2803u;
constexpr GlEnum kGlClampToEdge = 0x812Fu;
constexpr GlEnum kGlTexture0 = 0x84C0u;
constexpr GlEnum kGlDepthTest = 0x0B71u;
constexpr GlEnum kGlLess = 0x0201u;
constexpr GlEnum kGlColorBufferBit = 0x00004000u;
constexpr GlEnum kGlDepthBufferBit = 0x00000100u;
constexpr GlEnum kGlFront = 0x0404u;
constexpr GlEnum kGlVertexShader = 0x8B31u;
constexpr GlEnum kGlFragmentShader = 0x8B30u;
constexpr GlEnum kGlCompileStatus = 0x8B81u;
constexpr GlEnum kGlLinkStatus = 0x8B82u;
constexpr GlEnum kGlUnpackAlignment = 0x0CF5u;

struct Mat4 {
    float m[16]{};
};

Mat4 perspective(float fovYDegrees, float aspect, float nearPlane, float farPlane)
{
    const float f = 1.0f / std::tan(fovYDegrees * 3.14159265358979323846f / 360.0f);
    Mat4 out;
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    out.m[11] = -1.0f;
    out.m[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    return out;
}

Mat4 lookAt(kotor::Vec3 eye, kotor::Vec3 forward, kotor::Vec3 up)
{
    const kotor::Vec3 f = kotor::normalize(forward);
    const kotor::Vec3 s = kotor::normalize(kotor::cross(f, up));
    const kotor::Vec3 u = kotor::cross(s, f);
    Mat4 out;
    out.m[0] = s.x; out.m[4] = s.y; out.m[8] = s.z; out.m[12] = -kotor::dot(s, eye);
    out.m[1] = u.x; out.m[5] = u.y; out.m[9] = u.z; out.m[13] = -kotor::dot(u, eye);
    out.m[2] = -f.x; out.m[6] = -f.y; out.m[10] = -f.z; out.m[14] = kotor::dot(f, eye);
    out.m[3] = 0.0f; out.m[7] = 0.0f; out.m[11] = 0.0f; out.m[15] = 1.0f;
    return out;
}

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

Mat4 identity()
{
    Mat4 out;
    out.m[0] = 1.0f;
    out.m[5] = 1.0f;
    out.m[10] = 1.0f;
    out.m[15] = 1.0f;
    return out;
}

Mat4 translation(kotor::Vec3 p)
{
    Mat4 out = identity();
    out.m[12] = p.x;
    out.m[13] = p.y;
    out.m[14] = p.z;
    return out;
}

Mat4 rotationZ(float radians)
{
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 out = identity();
    out.m[0] = c;
    out.m[1] = s;
    out.m[4] = -s;
    out.m[5] = c;
    return out;
}

} // namespace

bool GlScene::initialize(std::string& error)
{
#define KE_LOAD_GL(member, name) \
    gl_.member = reinterpret_cast<decltype(gl_.member)>(SDL_GL_GetProcAddress(name)); \
    if (gl_.member == nullptr) { error = std::string("missing OpenGL entry point ") + name; return false; }
    KE_LOAD_GL(GenVertexArrays, "glGenVertexArrays")
    KE_LOAD_GL(BindVertexArray, "glBindVertexArray")
    KE_LOAD_GL(DeleteVertexArrays, "glDeleteVertexArrays")
    KE_LOAD_GL(GenBuffers, "glGenBuffers")
    KE_LOAD_GL(BindBuffer, "glBindBuffer")
    KE_LOAD_GL(BufferData, "glBufferData")
    KE_LOAD_GL(DeleteBuffers, "glDeleteBuffers")
    KE_LOAD_GL(EnableVertexAttribArray, "glEnableVertexAttribArray")
    KE_LOAD_GL(VertexAttribPointer, "glVertexAttribPointer")
    KE_LOAD_GL(CreateShader, "glCreateShader")
    KE_LOAD_GL(ShaderSource, "glShaderSource")
    KE_LOAD_GL(CompileShader, "glCompileShader")
    KE_LOAD_GL(GetShaderiv, "glGetShaderiv")
    KE_LOAD_GL(GetShaderInfoLog, "glGetShaderInfoLog")
    KE_LOAD_GL(CreateProgram, "glCreateProgram")
    KE_LOAD_GL(AttachShader, "glAttachShader")
    KE_LOAD_GL(LinkProgram, "glLinkProgram")
    KE_LOAD_GL(GetProgramiv, "glGetProgramiv")
    KE_LOAD_GL(GetProgramInfoLog, "glGetProgramInfoLog")
    KE_LOAD_GL(UseProgram, "glUseProgram")
    KE_LOAD_GL(DeleteShader, "glDeleteShader")
    KE_LOAD_GL(DeleteProgram, "glDeleteProgram")
    KE_LOAD_GL(GetUniformLocation, "glGetUniformLocation")
    KE_LOAD_GL(UniformMatrix4fv, "glUniformMatrix4fv")
    KE_LOAD_GL(Uniform3f, "glUniform3f")
    KE_LOAD_GL(Uniform1f, "glUniform1f")
    KE_LOAD_GL(Uniform1i, "glUniform1i")
    KE_LOAD_GL(ActiveTexture, "glActiveTexture")
    KE_LOAD_GL(GenTextures, "glGenTextures")
    KE_LOAD_GL(BindTexture, "glBindTexture")
    KE_LOAD_GL(TexImage2D, "glTexImage2D")
    KE_LOAD_GL(TexParameteri, "glTexParameteri")
    KE_LOAD_GL(DeleteTextures, "glDeleteTextures")
    KE_LOAD_GL(DrawElements, "glDrawElements")
    KE_LOAD_GL(Enable, "glEnable")
    KE_LOAD_GL(Disable, "glDisable")
    KE_LOAD_GL(DepthFunc, "glDepthFunc")
    KE_LOAD_GL(ClearDepth, "glClearDepth")
    KE_LOAD_GL(ClearColor, "glClearColor")
    KE_LOAD_GL(Clear, "glClear")
    KE_LOAD_GL(Viewport, "glViewport")
    KE_LOAD_GL(ReadBuffer, "glReadBuffer")
    KE_LOAD_GL(ReadPixels, "glReadPixels")
    KE_LOAD_GL(PixelStorei, "glPixelStorei")
    KE_LOAD_GL(GenerateMipmap, "glGenerateMipmap")
    KE_LOAD_GL(GetError, "glGetError")
#undef KE_LOAD_GL

    const char* vertexSource = R"GLSL(
#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
uniform mat4 uModel;
uniform mat4 uViewProjection;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vTexCoord;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vTexCoord = aTexCoord;
    gl_Position = uViewProjection * world;
}
)GLSL";
    const char* fragmentSource = R"GLSL(
#version 450 core
in vec3 vWorld;
in vec3 vNormal;
in vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec3 uAmbient;
uniform vec3 uDiffuse;
uniform vec3 uFogColor;
uniform float uFogNear;
uniform float uFogFar;
out vec4 fragColor;
void main() {
    vec4 texel = texture(uTexture, vTexCoord);
    vec3 normal = normalize(vNormal);
    float light = max(dot(normal, normalize(vec3(-0.35, -0.55, 0.75))), 0.0);
    vec3 color = texel.rgb * (uAmbient + uDiffuse * light);
    float distance = length(vWorld);
    float fog = clamp((distance - uFogNear) / max(uFogFar - uFogNear, 1.0), 0.0, 1.0);
    color = mix(color, uFogColor, fog);
    fragColor = vec4(color, texel.a);
}
)GLSL";

    const GlUint vertex = compileShader(kGlVertexShader, vertexSource, error);
    if (vertex == 0) {
        return false;
    }
    const GlUint fragment = compileShader(kGlFragmentShader, fragmentSource, error);
    if (fragment == 0) {
        gl_.DeleteShader(vertex);
        return false;
    }
    program_ = gl_.CreateProgram();
    gl_.AttachShader(program_, vertex);
    gl_.AttachShader(program_, fragment);
    gl_.LinkProgram(program_);
    GlInt linked = 0;
    gl_.GetProgramiv(program_, kGlLinkStatus, &linked);
    gl_.DeleteShader(vertex);
    gl_.DeleteShader(fragment);
    if (linked == 0) {
        char log[2048]{};
        gl_.GetProgramInfoLog(program_, static_cast<GlSizei>(sizeof(log)), nullptr, log);
        error = std::string("scene shader link failed: ") + log;
        gl_.DeleteProgram(program_);
        program_ = 0;
        return false;
    }
    return true;
}

GlUint GlScene::compileShader(GlEnum type, const char* source, std::string& error)
{
    const GlUint shader = gl_.CreateShader(type);
    gl_.ShaderSource(shader, 1, &source, nullptr);
    gl_.CompileShader(shader);
    GlInt compiled = 0;
    gl_.GetShaderiv(shader, kGlCompileStatus, &compiled);
    if (compiled == 0) {
        char log[2048]{};
        gl_.GetShaderInfoLog(shader, static_cast<GlSizei>(sizeof(log)), nullptr, log);
        error = std::string("scene shader compile failed: ") + log;
        gl_.DeleteShader(shader);
        return 0;
    }
    return shader;
}

void GlScene::destroyGpu()
{
    if (!textures_.empty()) {
        gl_.DeleteTextures(static_cast<GlSizei>(textures_.size()), textures_.data());
        textures_.clear();
    }
    for (GlMesh& mesh : meshes_) {
        if (mesh.ibo != 0) {
            gl_.DeleteBuffers(1, &mesh.ibo);
        }
        if (mesh.vbo != 0) {
            gl_.DeleteBuffers(1, &mesh.vbo);
        }
        if (mesh.vao != 0) {
            gl_.DeleteVertexArrays(1, &mesh.vao);
        }
    }
    meshes_.clear();
    scene_ = nullptr;
}

void GlScene::shutdown()
{
    destroyGpu();
    if (program_ != 0) {
        gl_.DeleteProgram(program_);
        program_ = 0;
    }
}

GlScene::GlMesh GlScene::uploadMesh(const std::vector<kotor::SceneVertex>& vertices,
                                    const std::vector<std::uint32_t>& indices,
                                    const std::vector<kotor::TextureBatch>& batches,
                                    std::uint32_t entity)
{
    GlMesh mesh;
    mesh.entity = entity;
    mesh.batches = batches;
    gl_.GenVertexArrays(1, &mesh.vao);
    gl_.BindVertexArray(mesh.vao);
    gl_.GenBuffers(1, &mesh.vbo);
    gl_.BindBuffer(kGlArrayBuffer, mesh.vbo);
    gl_.BufferData(kGlArrayBuffer, static_cast<GlSizeiptr>(vertices.size() * sizeof(kotor::SceneVertex)),
                   vertices.data(), kGlStaticDraw);
    gl_.GenBuffers(1, &mesh.ibo);
    gl_.BindBuffer(kGlElementArrayBuffer, mesh.ibo);
    gl_.BufferData(kGlElementArrayBuffer, static_cast<GlSizeiptr>(indices.size() * sizeof(std::uint32_t)),
                   indices.data(), kGlStaticDraw);
    gl_.EnableVertexAttribArray(0);
    gl_.VertexAttribPointer(0, 3, kGlFloat, 0, static_cast<GlSizei>(sizeof(kotor::SceneVertex)),
                            reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, position)));
    gl_.EnableVertexAttribArray(1);
    gl_.VertexAttribPointer(1, 3, kGlFloat, 0, static_cast<GlSizei>(sizeof(kotor::SceneVertex)),
                            reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, normal)));
    gl_.EnableVertexAttribArray(2);
    gl_.VertexAttribPointer(2, 2, kGlFloat, 0, static_cast<GlSizei>(sizeof(kotor::SceneVertex)),
                            reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, uv)));
    gl_.BindVertexArray(0);
    return mesh;
}

bool GlScene::upload(const kotor::Scene& scene, std::string& error)
{
    destroyGpu();
    if (program_ == 0) {
        error = "scene renderer is not initialized";
        return false;
    }
    scene_ = &scene;
    ambient_ = scene.ambient;
    diffuse_ = scene.diffuse;
    fogColor_ = scene.fogColor;
    fogNear_ = scene.fogNear;
    fogFar_ = scene.fogFar;

    textures_.resize(scene.textures.size());
    if (!textures_.empty()) {
        gl_.GenTextures(static_cast<GlSizei>(textures_.size()), textures_.data());
        gl_.PixelStorei(kGlUnpackAlignment, 1);
        for (std::size_t i = 0; i < scene.textures.size(); ++i) {
            const kotor::Image& image = scene.textures[i].image;
            gl_.BindTexture(kGlTexture2D, textures_[i]);
            gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<GlInt>(kGlLinear));
            gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<GlInt>(kGlLinear));
            gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS, static_cast<GlInt>(kGlClampToEdge));
            gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT, static_cast<GlInt>(kGlClampToEdge));
            gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), image.width, image.height, 0,
                           kGlRgba, kGlUnsignedByte, image.rgba.data());
        }
    }
    meshes_.push_back(uploadMesh(scene.vertices, scene.indices, scene.batches, UINT32_MAX));
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        meshes_.push_back(uploadMesh(entityMesh.vertices, entityMesh.indices, entityMesh.batches,
                                     entityMesh.entity));
    }
    gl_.BindVertexArray(0);
    const GlEnum uploadError = gl_.GetError();
    if (uploadError != 0) {
        logWarn(concat("OpenGL scene upload error 0x", std::hex, uploadError, std::dec));
    }
    return true;
}

void GlScene::render(const SceneCamera& camera, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    gl_.Viewport(0, 0, width, height);
    gl_.ClearColor(fogColor_.x, fogColor_.y, fogColor_.z, 1.0f);
    gl_.ClearDepth(1.0);
    gl_.Clear(kGlColorBufferBit | kGlDepthBufferBit);
    if (program_ == 0 || meshes_.empty()) {
        return;
    }
    gl_.Enable(kGlDepthTest);
    gl_.DepthFunc(kGlLess);
    gl_.UseProgram(program_);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Mat4 view = lookAt(camera.eye, camera.forward, camera.up);
    const Mat4 projection = perspective(camera.fovYDegrees, aspect, 0.05f, 500.0f);
    const Mat4 viewProjection = multiply(projection, view);
    gl_.UniformMatrix4fv(gl_.GetUniformLocation(program_, "uViewProjection"), 1, 0, viewProjection.m);
    gl_.Uniform3f(gl_.GetUniformLocation(program_, "uAmbient"), ambient_.x, ambient_.y, ambient_.z);
    gl_.Uniform3f(gl_.GetUniformLocation(program_, "uDiffuse"), diffuse_.x, diffuse_.y, diffuse_.z);
    gl_.Uniform3f(gl_.GetUniformLocation(program_, "uFogColor"), fogColor_.x, fogColor_.y, fogColor_.z);
    gl_.Uniform1f(gl_.GetUniformLocation(program_, "uFogNear"), fogNear_);
    gl_.Uniform1f(gl_.GetUniformLocation(program_, "uFogFar"), fogFar_);
    gl_.Uniform1i(gl_.GetUniformLocation(program_, "uTexture"), 0);
    const GlInt modelLocation = gl_.GetUniformLocation(program_, "uModel");
    std::size_t drawnMeshes = 0;
    for (const GlMesh& mesh : meshes_) {
        Mat4 model = identity();
        if (mesh.entity != UINT32_MAX && scene_ != nullptr && mesh.entity < scene_->entities.size()) {
            const kotor::Entity& entity = scene_->entities[mesh.entity];
            if (entity.dead) {
                continue;
            }
            model = multiply(translation(entity.position), rotationZ(entity.bearing));
        }
        gl_.UniformMatrix4fv(modelLocation, 1, 0, model.m);
        gl_.BindVertexArray(mesh.vao);
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.texture >= textures_.size() || batch.indexCount == 0) {
                continue;
            }
            gl_.ActiveTexture(kGlTexture0);
            gl_.BindTexture(kGlTexture2D, textures_[batch.texture]);
            gl_.DrawElements(kGlTriangles, static_cast<GlSizei>(batch.indexCount), kGlUnsignedInt,
                             reinterpret_cast<const void*>(static_cast<std::size_t>(batch.firstIndex) * sizeof(std::uint32_t)));
        }
        ++drawnMeshes;
    }
    gl_.BindVertexArray(0);
    if (!loggedFirstFrame_) {
        loggedFirstFrame_ = true;
        const GlEnum drawError = gl_.GetError();
        logInfo(concat("Scene frame: eye ", camera.eye.x, ", ", camera.eye.y, ", ", camera.eye.z,
                       " meshes ", drawnMeshes, "/", meshes_.size(), " textures ", textures_.size(),
                       drawError != 0 ? concat(" glError 0x", std::hex, drawError, std::dec).c_str() : ""));
    }
}

bool GlScene::screenshot(const std::filesystem::path& path, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u, 0u);
    gl_.ReadBuffer(kGlFront);
    gl_.PixelStorei(kGlUnpackAlignment, 1);
    gl_.ReadPixels(0, 0, width, height, kGlRgba, kGlUnsignedByte, pixels.data());
    return writeBmp(path, width, height, pixels);
}

} // namespace ke
