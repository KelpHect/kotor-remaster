#include "render/GlScene.h"
#include "render/Bmp.h"

#include "core/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <unordered_map>
#include <vector>

namespace ke {

namespace {

constexpr GlEnum kGlArrayBuffer = 0x8892u;
constexpr GlEnum kGlElementArrayBuffer = 0x8893u;
constexpr GlEnum kGlStaticDraw = 0x88E4u;
constexpr GlEnum kGlDynamicDraw = 0x88E8u;
constexpr GlEnum kGlFloat = 0x1406u;
constexpr GlEnum kGlTriangles = 0x0004u;
constexpr GlEnum kGlBlend = 0x0BE2u;
constexpr GlEnum kGlSrcAlpha = 0x0302u;
constexpr GlEnum kGlOneMinusSrcAlpha = 0x0303u;
constexpr GlEnum kGlOne = 0x0001u;
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
constexpr GlEnum kGlLequal = 0x0203u; // retail's one glDepthFunc call (at 004513a6) passes 0073f2ac: GL_LEQUAL
constexpr GlEnum kGlColorBufferBit = 0x00004000u;
constexpr GlEnum kGlDepthBufferBit = 0x00000100u;
constexpr GlEnum kGlFront = 0x0404u;
constexpr GlEnum kGlVertexShader = 0x8B31u;
constexpr GlEnum kGlFragmentShader = 0x8B30u;
constexpr GlEnum kGlCompileStatus = 0x8B81u;
constexpr GlEnum kGlLinkStatus = 0x8B82u;
constexpr GlEnum kGlUnpackAlignment = 0x0CF5u;
constexpr GlEnum kGlRepeat = 0x2901u;
constexpr GlEnum kGlTexture1 = 0x84C1u;
constexpr GlEnum kGlScissorTest = 0x0C11u;
constexpr GlEnum kGlBack = 0x0405u;
constexpr GlEnum kGlPackAlignment = 0x0D05u;
constexpr GlEnum kGlCullFace = 0x0B44u;
constexpr GlEnum kGlLinearMipmapLinear = 0x2703u;
constexpr GlEnum kGlTexture2 = 0x84C2u;
constexpr GlEnum kGlTexture3 = 0x84C3u;

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
    KE_LOAD_GL(BufferSubData, "glBufferSubData")
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
    KE_LOAD_GL(BlendFunc, "glBlendFunc")
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
    KE_LOAD_GL(DrawArrays, "glDrawArrays")
    KE_LOAD_GL(CopyTexSubImage2D, "glCopyTexSubImage2D")
    KE_LOAD_GL(Uniform3fv, "glUniform3fv")
    KE_LOAD_GL(Scissor, "glScissor")
    KE_LOAD_GL(CullFace, "glCullFace")
    KE_LOAD_GL(DepthMask, "glDepthMask")
    KE_LOAD_GL(Uniform4f, "glUniform4f")
    KE_LOAD_GL(ColorMask, "glColorMask")
    KE_LOAD_GL(StencilFunc, "glStencilFunc")
    KE_LOAD_GL(StencilOp, "glStencilOp")
    KE_LOAD_GL(StencilMask, "glStencilMask")
    KE_LOAD_GL(ClearStencil, "glClearStencil")
    KE_LOAD_GL(FrontFace, "glFrontFace")
#undef KE_LOAD_GL

    const char* vertexSource = R"GLSL(
#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aTexCoord1;
layout(location = 4) in vec4 aTint;
uniform mat4 uModel;
uniform mat4 uViewProjection;
uniform vec4 uParams; // w 3: a force shell, moved out along the normal (shaders/scene.vert)
uniform vec4 uBump;
uniform float uBumpSpecular;
uniform vec3 uAmbient;
uniform vec4 uLightPosition[3];
uniform vec3 uLightColour[3];
out vec3 vWorld;
out vec3 vNormal;
out vec2 vTexCoord;
out vec2 vTexCoord1;
out vec4 vTint;
out vec3 vLit; // lit per vertex, as retail's fixed-function GL (shaders/scene.vert)
void main() {
    vec4 world = uModel * vec4(uParams.w > 2.5 && uParams.w < 3.5 ? aPosition + aNormal * 0.02 : aPosition, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vTexCoord = aTexCoord;
    vTexCoord1 = aTexCoord1;
    vTint = aTint;
    vLit = vec3(0.0);
    if (uParams.w < 0.5 && uParams.y < 0.5 && uBump.x < 0.5 && uBumpSpecular <= 0.0) {
        vec3 normal = normalize(vNormal);
        vec3 light = uAmbient;
        for (int i = 0; i < 3; ++i) {
            vec3 toLight = uLightPosition[i].xyz;
            float attenuation = 1.0;
            if (uLightPosition[i].w >= 0.0) {
                toLight -= world.xyz;
                attenuation = 1.0 / (1.0 + uLightPosition[i].w * dot(toLight, toLight));
                toLight = normalize(toLight);
            }
            light += uLightColour[i] * attenuation * max(dot(normal, toLight), 0.0);
        }
        vLit = min(light + aTint.rgb, vec3(1.0));
    }
    gl_Position = uViewProjection * world;
}
)GLSL";
    // Same maths as shaders/scene.frag.
    const char* fragmentSource = R"GLSL(
#version 450 core
in vec3 vWorld;
in vec3 vNormal;
in vec2 vTexCoord;
in vec2 vTexCoord1;
in vec4 vTint;
in vec3 vLit;
uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform sampler2D uEnvMap;
uniform sampler2D uBumpMap;
uniform vec4 uBump; // bump cells x, y and cell x, y; x 0 = none
uniform float uBumpSpecular;
uniform vec3 uAmbient;
uniform vec4 uLightPosition[3];
uniform vec3 uLightColour[3];
uniform vec3 uFogColor;
uniform vec3 uEye;
uniform float uFogNear;
uniform float uFogFar;
uniform float uFogOn;
uniform vec4 uUv;
uniform vec4 uParams; // alpha reference, lightmap, environment map, unlit (4: distortion)
out vec4 fragColor;
// ProgFX 1800 distortion, same maths as shaders/scene.frag. The ring texture (distortiontex.txi:
// ringtexdistort 32 x 32, built by 00460a90): texel (x, y) holds 255 x (0.5 + 0.03 x cos32(25 phase +
// 60 r)), r the distance of ((x + 1) / 32 - 0.5, (y + 1) / 32 - 0.5) from 0, cos32 the 32-step table
// 004aadf0 / 00743fe8; red and green alike, blue 255. These are its 32 possible bytes.
const float kRingBytes[32] = float[32](135.0, 135.0, 134.0, 133.0, 132.0, 131.0, 130.0, 128.0, 127.0, 126.0,
    124.0, 123.0, 122.0, 121.0, 120.0, 119.0, 119.0, 119.0, 120.0, 121.0, 122.0, 123.0, 124.0, 126.0, 127.0,
    128.0, 130.0, 131.0, 132.0, 133.0, 134.0, 135.0);
float ringTexel(ivec2 p, float phase) {
    vec2 f = vec2(p + 1) / 32.0 - 0.5;
    float a = 25.0 * phase + 60.0 * sqrt(f.x * f.x + f.y * f.y);
    return kRingBytes[int(roundEven(a * 5.09295797)) & 31] / 255.0;
}
// GL_LINEAR, clamped (RenderForceDistortion sets GL_CLAMP on the unit; drivers clamp to the edge).
float ringSample(vec2 st, float phase) {
    vec2 p = st * 32.0 - 0.5;
    vec2 f = p - floor(p);
    ivec2 a = clamp(ivec2(floor(p)), ivec2(0), ivec2(31));
    ivec2 b = clamp(ivec2(floor(p)) + 1, ivec2(0), ivec2(31));
    float near = mix(ringTexel(a, phase), ringTexel(ivec2(b.x, a.y), phase), f.x);
    float far = mix(ringTexel(ivec2(a.x, b.y), phase), ringTexel(b, phase), f.x);
    return mix(near, far, f.y);
}
void main() {
    vec4 texel = texture(uTexture, vTexCoord * uUv.xy + uUv.zw);
    if (uParams.x >= 0.0 && texel.a <= uParams.x) {
        discard;
    }
    if (uParams.w > 3.5) {
        // Vertex program 0078de48 and the texture shader / ATI shader of 004331d0 / 00432d70: the
        // frame is read at (0.1 w nx + x, 0.1 h ny + y) with n the expanded ring texel (nz = 1), the
        // ring stretched once over the screen from its top left. uLightPosition[0] = 1/w, 1/h, phase.
        vec2 uv = gl_FragCoord.xy * uLightPosition[0].xy;
        float n = ringSample(vec2(uv.x, 1.0 - uv.y), uLightPosition[0].z) * 2.0 - 1.0;
        fragColor = vec4(texture(uEnvMap, uv + 0.1 * n).rgb, 1.0);
        return;
    }
    vec3 normal = normalize(vNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    if (uBump.x > 0.5) {
        vec2 cells = uBump.xy;
        vec2 cell = uBump.zw;
        vec3 mapNormal = textureGrad(uBumpMap, (fract(vTexCoord) + cell) / cells, dFdx(vTexCoord) / cells, dFdy(vTexCoord) / cells).rgb * 2.0 - 1.0;
        vec3 dp1 = dFdx(vWorld);
        vec3 dp2 = dFdy(vWorld);
        vec2 d1 = dFdx(vTexCoord);
        vec2 d2 = dFdy(vTexCoord);
        float det = d1.x * d2.y - d2.x * d1.y;
        if (det != 0.0) {
            float s = det < 0.0 ? -1.0 : 1.0;
            vec3 t = (dp1 * d2.y - dp2 * d1.y) * s;
            vec3 b = (dp2 * d1.x - dp1 * d2.x) * s;
            t -= normal * dot(normal, t);
            b -= normal * dot(normal, b);
            if (dot(t, t) > 0.0 && dot(b, b) > 0.0) {
                normal = normalize(normalize(t) * mapNormal.x + normalize(b) * mapNormal.y + normal * mapNormal.z);
            }
        }
    }
    vec3 color;
    vec3 litTerm = vec3(1.0);
    float alpha = texel.a * vTint.a;
    if (uParams.w > 2.5) {
        color = texel.rgb;
        alpha = texel.a;
    } else if (uParams.w > 0.5) {
        litTerm = vTint.rgb;
        color = texel.rgb * vTint.rgb;
    } else if (uParams.y < 0.5 && uBump.x < 0.5 && uBumpSpecular <= 0.0) {
        litTerm = vLit;
        color = texel.rgb * vLit;
    } else {
        vec3 light;
        vec3 specular = vec3(0.0);
        if (uParams.y > 0.5) {
            light = texture(uLightmap, vTexCoord1).rgb;
        } else {
            light = uAmbient;
            for (int i = 0; i < 3; ++i) {
                vec3 toLight = uLightPosition[i].xyz;
                float attenuation = 1.0;
                if (uLightPosition[i].w >= 0.0) {
                    toLight -= vWorld;
                    attenuation = 1.0 / (1.0 + uLightPosition[i].w * dot(toLight, toLight));
                    toLight = normalize(toLight);
                }
                light += uLightColour[i] * attenuation * max(dot(normal, toLight), 0.0);
                if (uBumpSpecular > 0.0) {
                    // Program 830250: attenuation * con0 * (N.H)^8 * (1 - texel alpha).
                    float nh = max(dot(normal, normalize(toLight + normalize(uEye - vWorld))), 0.0);
                    specular += uLightColour[i] * attenuation * uBumpSpecular * pow(nh, 8.0);
                }
            }
        }
        // The exe sets no colour scale (no RGB_SCALE push), so a lit colour tops out at the texel.
        litTerm = min(light + vTint.rgb, vec3(1.0));
        color = texel.rgb * litTerm + specular * (1.0 - texel.a);
    }
    if (uParams.z > 0.5) {
        vec3 r = reflect(normalize(vWorld - uEye), normal);
        vec2 ll = vec2(atan(r.y, r.x) / 6.2831853 + 0.5, acos(clamp(r.z, -1.0, 1.0)) / 3.14159265);
        vec3 env = texture(uEnvMap, ll).rgb;
        if (uParams.z > 1.5) {
            color = env;
            alpha = vTint.a * (uParams.z - 2.0);
        } else {
            // Retail ATI programs: 830258 with a lightmap adds the environment. The bump route 00470860 adds
            // with or without a lightmap. Without a lightmap, retail's route without fragment shaders (0046ff70,
            // the one the retail captures take, as the AMD driver faults in GL_ATI_fragment_shader) draws the mesh
            // twice: the cube map alone under the vertex lighting (texture env MODULATE), then the texture over it
            // with SRC_ALPHA / ONE_MINUS_SRC_ALPHA (0073f270). So the reflection is lit too. Paired on the swoop
            // bike (paired\minigame_swoop_env_pair.png).
            color = uParams.y > 0.5 || uBump.x > 0.5 ? color + env * (1.0 - texel.a) : mix(env * (uParams.z > 1.1 ? vec3(1.0) : litTerm), color, texel.a);
            alpha = vTint.a;
        }
    }
    if (uParams.w > 1.5 && uParams.w < 2.5) {
        alpha = 1.0;
    }
    if (uFogOn > 0.5) {
        float fog = clamp((length(vWorld - uEye) - uFogNear) / max(uFogFar - uFogNear, 1.0), 0.0, 1.0);
        color = mix(color, uFogColor, fog);
    }
    fragColor = vec4(color, alpha);
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
#define KE_UNIFORM(field, name) uniforms_.field = gl_.GetUniformLocation(program_, name);
    KE_UNIFORM(model, "uModel")
    KE_UNIFORM(viewProjection, "uViewProjection")
    KE_UNIFORM(ambient, "uAmbient")
    for (int i = 0; i < 3; ++i) {
        const std::string index = "[" + std::to_string(i) + "]";
        uniforms_.lightPosition[static_cast<std::size_t>(i)] = gl_.GetUniformLocation(program_, ("uLightPosition" + index).c_str());
        uniforms_.lightColour[static_cast<std::size_t>(i)] = gl_.GetUniformLocation(program_, ("uLightColour" + index).c_str());
    }
    KE_UNIFORM(fogColor, "uFogColor")
    KE_UNIFORM(eye, "uEye")
    KE_UNIFORM(fogNear, "uFogNear")
    KE_UNIFORM(fogFar, "uFogFar")
    KE_UNIFORM(fogOn, "uFogOn")
    KE_UNIFORM(uv, "uUv")
    KE_UNIFORM(params, "uParams")
    KE_UNIFORM(texture, "uTexture")
    KE_UNIFORM(lightmap, "uLightmap")
    KE_UNIFORM(envMap, "uEnvMap")
    KE_UNIFORM(bumpMap, "uBumpMap")
    KE_UNIFORM(bump, "uBump")
    KE_UNIFORM(bumpSpecular, "uBumpSpecular")
#undef KE_UNIFORM
    std::string postError;
    if (!createPostProgram(postError)) {
        logWarn(concat("Video effect pass unavailable on OpenGL: ", postError));
    }
    std::string shadowError;
    if (!createShadowProgram(shadowError)) {
        logWarn(concat("Stencil shadows unavailable on OpenGL: ", shadowError));
    }
    return true;
}

// Stencil shadows: the volumes write only the stencil, the fan is black with its vertex alpha
// (00450730 draws it with texturing and lighting off, 00451a50), fogged as the scene's fragments.
// Same maths as shaders/shadow.vert and shadow.frag (Vulkan).
bool GlScene::createShadowProgram(std::string& error)
{
    const char* vertexSource = R"GLSL(
#version 450 core
layout(location = 0) in vec4 aPoint; // xyz, w = alpha
uniform mat4 uViewProjection;
out vec3 vWorld;
out float vAlpha;
void main() {
    vWorld = aPoint.xyz;
    vAlpha = aPoint.w;
    gl_Position = uViewProjection * vec4(aPoint.xyz, 1.0);
}
)GLSL";
    const char* fragmentSource = R"GLSL(
#version 450 core
in vec3 vWorld;
in float vAlpha;
uniform vec4 uFog; // rgb, w = fog on
uniform vec4 uEye; // xyz, w = fog near
uniform float uFogFar;
out vec4 fragColor;
void main() {
    vec3 color = vec3(0.0);
    if (uFog.w > 0.5) {
        float fog = clamp((length(vWorld - uEye.xyz) - uEye.w) / max(uFogFar - uEye.w, 1.0), 0.0, 1.0);
        color = mix(color, uFog.rgb, fog);
    }
    fragColor = vec4(color, vAlpha);
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
    shadowProgram_ = gl_.CreateProgram();
    gl_.AttachShader(shadowProgram_, vertex);
    gl_.AttachShader(shadowProgram_, fragment);
    gl_.LinkProgram(shadowProgram_);
    GlInt linked = 0;
    gl_.GetProgramiv(shadowProgram_, kGlLinkStatus, &linked);
    gl_.DeleteShader(vertex);
    gl_.DeleteShader(fragment);
    if (linked == 0) {
        char log[2048]{};
        gl_.GetProgramInfoLog(shadowProgram_, static_cast<GlSizei>(sizeof(log)), nullptr, log);
        error = std::string("shadow shader link failed: ") + log;
        gl_.DeleteProgram(shadowProgram_);
        shadowProgram_ = 0;
        return false;
    }
    shadowViewProjection_ = gl_.GetUniformLocation(shadowProgram_, "uViewProjection");
    shadowFog_ = gl_.GetUniformLocation(shadowProgram_, "uFog");
    shadowEye_ = gl_.GetUniformLocation(shadowProgram_, "uEye");
    shadowFogFar_ = gl_.GetUniformLocation(shadowProgram_, "uFogFar");
    gl_.GenVertexArrays(1, &shadowVao_);
    gl_.GenBuffers(1, &shadowVbo_);
    gl_.BindVertexArray(shadowVao_);
    gl_.BindBuffer(kGlArrayBuffer, shadowVbo_);
    gl_.EnableVertexAttribArray(0);
    gl_.VertexAttribPointer(0, 4, kGlFloat, 0, static_cast<GlSizei>(4 * sizeof(float)), nullptr);
    gl_.BindVertexArray(0);
    gl_.BindBuffer(kGlArrayBuffer, 0);
    return true;
}

bool GlScene::createPostProgram(std::string& error)
{
    const char* vertexSource = R"GLSL(
#version 450 core
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
    // Same maths as shaders/effect.frag (Vulkan) and applyVideoEffectCpu. The
    // framebuffer copy has v = 0 at the bottom, so the screen-down coordinate
    // used for the scan lines is 1 - v.
    const char* fragmentSource = R"GLSL(
#version 450 core
in vec2 vUv;
uniform sampler2D uScene;
uniform sampler2D uNoise;
uniform vec3 uRow0;
uniform vec3 uRow1;
uniform vec3 uRow2;
uniform vec3 uNoiseScale;
uniform vec3 uNoiseTranslate;
uniform int uSaturation;
uniform int uScan;
uniform sampler2D uAccum;
uniform int uBlur;
uniform float uBlurRatio;
out vec4 fragColor;
void main() {
    vec3 c = texture(uScene, vUv).rgb;
    if (uBlur != 0) {
        // Speed blur, register combiners 0x128 (004299c0): spare0 = last x ratio, + frame x (1 - ratio).
        c = texture(uAccum, vUv).rgb * uBlurRatio + c * (1.0 - uBlurRatio);
    }
    vec3 o = c;
    if (uSaturation != 0) {
        o = vec3(dot(uRow0, c), dot(uRow1, c), dot(uRow2, c));
    }
    if (uScan != 0) {
        float t = (1.0 - vUv.y) * 7.5;
        o += texture(uNoise, vec2(0.0, t)).rgb * uNoiseScale + uNoiseTranslate;
    }
    fragColor = vec4(clamp(o, 0.0, 1.0), 1.0);
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
    postProgram_ = gl_.CreateProgram();
    gl_.AttachShader(postProgram_, vertex);
    gl_.AttachShader(postProgram_, fragment);
    gl_.LinkProgram(postProgram_);
    GlInt linked = 0;
    gl_.GetProgramiv(postProgram_, kGlLinkStatus, &linked);
    gl_.DeleteShader(vertex);
    gl_.DeleteShader(fragment);
    if (linked == 0) {
        char log[2048]{};
        gl_.GetProgramInfoLog(postProgram_, static_cast<GlSizei>(sizeof(log)), nullptr, log);
        error = std::string("video effect shader link failed: ") + log;
        gl_.DeleteProgram(postProgram_);
        postProgram_ = 0;
        return false;
    }
    gl_.GenVertexArrays(1, &postVao_);
    return true;
}

void GlScene::ensureSceneCopy(int width, int height)
{
    if (sceneCopy_ == 0 || copyWidth_ != width || copyHeight_ != height) {
        if (sceneCopy_ == 0) {
            gl_.GenTextures(1, &sceneCopy_);
        }
        gl_.BindTexture(kGlTexture2D, sceneCopy_);
        gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<GlInt>(kGlLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<GlInt>(kGlLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS, static_cast<GlInt>(kGlClampToEdge));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT, static_cast<GlInt>(kGlClampToEdge));
        gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), width, height, 0, kGlRgba,
                       kGlUnsignedByte, nullptr);
        copyWidth_ = width;
        copyHeight_ = height;
    }
    if (noiseTexture_ == 0) {
        const std::vector<std::uint8_t> noise = makeScanNoiseTexture();
        gl_.GenTextures(1, &noiseTexture_);
        gl_.BindTexture(kGlTexture2D, noiseTexture_);
        gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<GlInt>(kGlLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<GlInt>(kGlLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS, static_cast<GlInt>(kGlRepeat));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT, static_cast<GlInt>(kGlRepeat));
        gl_.PixelStorei(kGlUnpackAlignment, 1);
        gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), kScanNoiseSize, kScanNoiseSize,
                       0, kGlRgba, kGlUnsignedByte, noise.data());
    }
    gl_.ActiveTexture(kGlTexture0);
    gl_.BindTexture(kGlTexture2D, sceneCopy_);
    // The copy must come from the frame just drawn. screenshot() reads the front buffer
    // and used to leave it as the read buffer, so the next effect frame copied the
    // previously presented (already processed) image and compounded the effect.
    gl_.ReadBuffer(kGlBack);
    gl_.CopyTexSubImage2D(kGlTexture2D, 0, 0, 0, 0, 0, width, height);
}

void GlScene::applySpeedBlur(int width, int height)
{
    // FrameBufferModifications 00437520 (NV) / 00435bd0 (ATI), flag 0x10.
    if (!effect_.blurring() || postProgram_ == 0 || width <= 0 || height <= 0) {
        blurPrimed_ = false; // 007a698d = 0 whenever the blur block is skipped
        return;
    }
    if (blurAccum_ == 0 || blurWidth_ != width || blurHeight_ != height) {
        if (blurAccum_ == 0) {
            gl_.GenTextures(1, &blurAccum_);
        }
        gl_.BindTexture(kGlTexture2D, blurAccum_);
        gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<GlInt>(kGlLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<GlInt>(kGlLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS, static_cast<GlInt>(kGlClampToEdge));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT, static_cast<GlInt>(kGlClampToEdge));
        gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), width, height, 0, kGlRgba,
                       kGlUnsignedByte, nullptr);
        blurWidth_ = width;
        blurHeight_ = height;
        blurPrimed_ = false;
    }
    if (blurPrimed_) {
        // The frame to a texture, then a full-screen quad of last x ratio + frame x (1 - ratio) with
        // blending off (glBlendFunc(1, 0)); RenderAccumulationBlur 00433da0 / 00433970.
        ensureSceneCopy(width, height);
        gl_.ActiveTexture(kGlTexture2);
        gl_.BindTexture(kGlTexture2D, blurAccum_);
        gl_.ActiveTexture(kGlTexture0);
        gl_.Disable(kGlDepthTest);
        gl_.Disable(kGlBlend);
        gl_.UseProgram(postProgram_);
        gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uScene"), 0);
        gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uNoise"), 1);
        gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uAccum"), 2);
        gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uBlur"), 1);
        gl_.Uniform1f(gl_.GetUniformLocation(postProgram_, "uBlurRatio"), effect_.speedBlurRatio);
        gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uSaturation"), 0);
        gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uScan"), 0);
        gl_.BindVertexArray(postVao_);
        gl_.DrawArrays(kGlTriangles, 0, 3);
        gl_.BindVertexArray(0);
        gl_.UseProgram(0);
        gl_.Enable(kGlDepthTest);
    }
    // The (blurred) frame becomes the last frame: RenderFrameToTexture into the pbuffer, 007a698d = 1.
    gl_.ActiveTexture(kGlTexture0);
    gl_.BindTexture(kGlTexture2D, blurAccum_);
    gl_.ReadBuffer(kGlBack);
    gl_.CopyTexSubImage2D(kGlTexture2D, 0, 0, 0, 0, 0, width, height);
    gl_.BindTexture(kGlTexture2D, 0);
    blurPrimed_ = true;
}

void GlScene::applyDistortion(const SceneCamera& camera, int width, int height)
{
    // FrameBufferModifications flag 0x20 (set at 00451960 while the distortion list 007fbfc0 is not
    // empty): the frame is copied, then RenderForceDistortion 004331d0 redraws it where the flashing
    // models stand (their stencil from Scene::DoDistortionBuckets 004503a0), each pixel taken from the
    // frame moved by the ring texture. Here the models are drawn again with the scene program in its
    // distortion mode, which marks the same pixels.
    if (!effect_.distorting() || program_ == 0 || width <= 0 || height <= 0 || scene_ == nullptr) {
        return;
    }
    ensureSceneCopy(width, height);
    gl_.UseProgram(program_);
    const Mat4 viewProjection = multiply(perspective(camera.fovYDegrees, static_cast<float>(width) / static_cast<float>(height),
                                                     camera.nearClip, camera.farClip),
                                         lookAt(camera.eye, camera.forward, camera.up));
    gl_.UniformMatrix4fv(uniforms_.viewProjection, 1, 0, viewProjection.m);
    gl_.Uniform3f(uniforms_.eye, camera.eye.x, camera.eye.y, camera.eye.z);
    gl_.Uniform1i(uniforms_.texture, 0);
    gl_.Uniform1i(uniforms_.lightmap, 1);
    gl_.Uniform1i(uniforms_.envMap, 2);
    gl_.Uniform1i(uniforms_.bumpMap, 3);
    gl_.Uniform1f(uniforms_.fogOn, 0.0f);
    gl_.Uniform4f(uniforms_.bump, 0.0f, 1.0f, 0.0f, 0.0f);
    gl_.Uniform1f(uniforms_.bumpSpecular, 0.0f);
    gl_.Uniform4f(uniforms_.lightPosition[0], 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height),
                  effect_.distortPhase, 0.0f);
    gl_.ActiveTexture(kGlTexture2);
    gl_.BindTexture(kGlTexture2D, sceneCopy_);
    gl_.ActiveTexture(kGlTexture0);
    gl_.Disable(kGlBlend);
    gl_.Enable(kGlDepthTest);
    gl_.DepthMask(1);
    for (const GlMesh& mesh : meshes_) {
        if (!mesh.visible || mesh.fx || mesh.entity == UINT32_MAX || mesh.entity >= scene_->entities.size()
            || !effect_.hides(mesh.entity)) {
            continue;
        }
        const kotor::Entity& entity = scene_->entities[mesh.entity];
        if (entity.dead && !entity.corpse()) {
            continue;
        }
        const Mat4 model = multiply(translation(entity.position), rotationZ(entity.drawBearing()));
        gl_.UniformMatrix4fv(uniforms_.model, 1, 0, model.m);
        gl_.BindVertexArray(mesh.vao);
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.shell != 0u || batch.texture >= textures_.size() || batch.indexCount == 0) {
                continue;
            }
            const kotor::BatchUv uv = kotor::batchUvAt(batch, scene_->clock);
            gl_.Uniform4f(uniforms_.uv, uv.scaleU, uv.scaleV, uv.offsetU, uv.offsetV);
            gl_.Uniform4f(uniforms_.params, kotor::batchAlphaRef(batch, camera.eye), 0.0f, 0.0f, 4.0f);
            gl_.BindTexture(kGlTexture2D, textures_[batch.texture]);
            if (batch.twoSided) {
                gl_.Disable(kGlCullFace);
            } else {
                gl_.Enable(kGlCullFace);
            }
            gl_.DrawElements(kGlTriangles, static_cast<GlSizei>(batch.indexCount), kGlUnsignedInt,
                             reinterpret_cast<const void*>(static_cast<std::size_t>(batch.firstIndex) * sizeof(std::uint32_t)));
        }
    }
    gl_.Disable(kGlCullFace);
    gl_.BindVertexArray(0);
    gl_.ActiveTexture(kGlTexture2);
    gl_.BindTexture(kGlTexture2D, 0);
    gl_.ActiveTexture(kGlTexture0);
    gl_.UseProgram(0);
}

void GlScene::applyVideoEffect(int width, int height)
{
    if (!effect_.active() || postProgram_ == 0 || width <= 0 || height <= 0) {
        return;
    }
    ensureSceneCopy(width, height);
    gl_.ActiveTexture(kGlTexture1);
    gl_.BindTexture(kGlTexture2D, noiseTexture_);
    gl_.ActiveTexture(kGlTexture0);

    gl_.Disable(kGlDepthTest);
    gl_.Disable(kGlBlend);
    gl_.UseProgram(postProgram_);
    gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uScene"), 0);
    gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uNoise"), 1);
    const auto& m = effect_.matrix;
    gl_.Uniform3fv(gl_.GetUniformLocation(postProgram_, "uRow0"), 1, &m[0]);
    gl_.Uniform3fv(gl_.GetUniformLocation(postProgram_, "uRow1"), 1, &m[3]);
    gl_.Uniform3fv(gl_.GetUniformLocation(postProgram_, "uRow2"), 1, &m[6]);
    gl_.Uniform3fv(gl_.GetUniformLocation(postProgram_, "uNoiseScale"), 1, effect_.noiseScale.data());
    gl_.Uniform3fv(gl_.GetUniformLocation(postProgram_, "uNoiseTranslate"), 1,
                   effect_.noiseTranslate.data());
    gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uSaturation"), effect_.saturation ? 1 : 0);
    gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uScan"), effect_.scanNoise ? 1 : 0);
    gl_.Uniform1i(gl_.GetUniformLocation(postProgram_, "uBlur"), 0);
    gl_.BindVertexArray(postVao_);
    gl_.DrawArrays(kGlTriangles, 0, 3);
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    gl_.BindTexture(kGlTexture2D, 0);
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

void GlScene::deleteMesh(GlMesh& mesh)
{
    if (mesh.ibo != 0) {
        gl_.DeleteBuffers(1, &mesh.ibo);
    }
    if (mesh.vbo != 0) {
        gl_.DeleteBuffers(1, &mesh.vbo);
    }
    if (mesh.vao != 0) {
        gl_.DeleteVertexArrays(1, &mesh.vao);
    }
    mesh.ibo = 0;
    mesh.vbo = 0;
    mesh.vao = 0;
}

void GlScene::destroyGpu()
{
    if (!textures_.empty()) {
        gl_.DeleteTextures(static_cast<GlSizei>(textures_.size()), textures_.data());
        textures_.clear();
    }
    for (GlMesh& mesh : meshes_) {
        deleteMesh(mesh);
    }
    meshes_.clear();
    if (particleVbo_ != 0) {
        gl_.DeleteBuffers(1, &particleVbo_);
        particleVbo_ = 0;
    }
    if (particleVao_ != 0) {
        gl_.DeleteVertexArrays(1, &particleVao_);
        particleVao_ = 0;
    }
    scene_ = nullptr;
}

void GlScene::shutdown()
{
    destroyGpu();
    if (program_ != 0) {
        gl_.DeleteProgram(program_);
        program_ = 0;
    }
    if (postProgram_ != 0) {
        gl_.DeleteProgram(postProgram_);
        postProgram_ = 0;
    }
    if (postVao_ != 0) {
        gl_.DeleteVertexArrays(1, &postVao_);
        postVao_ = 0;
    }
    if (shadowProgram_ != 0) {
        gl_.DeleteProgram(shadowProgram_);
        shadowProgram_ = 0;
    }
    if (shadowVbo_ != 0) {
        gl_.DeleteBuffers(1, &shadowVbo_);
        shadowVbo_ = 0;
    }
    if (shadowVao_ != 0) {
        gl_.DeleteVertexArrays(1, &shadowVao_);
        shadowVao_ = 0;
    }
    if (sceneCopy_ != 0) {
        gl_.DeleteTextures(1, &sceneCopy_);
        sceneCopy_ = 0;
    }
    if (noiseTexture_ != 0) {
        gl_.DeleteTextures(1, &noiseTexture_);
        noiseTexture_ = 0;
    }
    if (blurAccum_ != 0) {
        gl_.DeleteTextures(1, &blurAccum_);
        blurAccum_ = 0;
    }
    blurWidth_ = 0;
    blurHeight_ = 0;
    blurPrimed_ = false;
    copyWidth_ = 0;
    copyHeight_ = 0;
}

void GlScene::bindExtraAttributes()
{
    gl_.EnableVertexAttribArray(3);
    gl_.VertexAttribPointer(3, 2, kGlFloat, 0, static_cast<GlSizei>(sizeof(kotor::SceneVertex)),
                            reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, uv1)));
    gl_.EnableVertexAttribArray(4);
    gl_.VertexAttribPointer(4, 4, kGlFloat, 0, static_cast<GlSizei>(sizeof(kotor::SceneVertex)),
                            reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, selfIllum)));
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
    bindExtraAttributes();
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
    fogOn_ = scene.fogOn;

    uploadTextures(scene, 0u);
    meshes_.push_back(uploadMesh(scene.vertices, scene.indices, scene.batches, UINT32_MAX));
    meshes_.back().vertexCount = scene.vertices.size();
    meshes_.back().indexCount = scene.indices.size();
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        meshes_.push_back(uploadMesh(entityMesh.vertices, entityMesh.indices, entityMesh.batches,
                                     entityMesh.entity));
        meshes_.back().visible = entityMesh.visible;
        meshes_.back().fx = entityMesh.fx;
        meshes_.back().uid = entityMesh.uid;
        meshes_.back().vertexCount = entityMesh.vertices.size();
        meshes_.back().indexCount = entityMesh.indices.size();
    }
    gl_.BindVertexArray(0);
    const GlEnum uploadError = gl_.GetError();
    if (uploadError != 0) {
        logWarn(concat("OpenGL scene upload error 0x", std::hex, uploadError, std::dec));
    }
    return true;
}

// An animated texture rewrote its image (retail 00462520 and 00461820 upload through the
// texture's vfunc +0xac).
void GlScene::refreshProceduralTextures()
{
    if (scene_ == nullptr || scene_->procedurals.empty()) {
        return;
    }
    textureRevisions_.resize(textures_.size(), 0u);
    gl_.PixelStorei(kGlUnpackAlignment, 1);
    for (const kotor::ProceduralTexture& state : scene_->procedurals) {
        if (state.texture >= textures_.size() || state.texture >= scene_->textures.size()) {
            continue;
        }
        const kotor::Texture& texture = scene_->textures[state.texture];
        if (textureRevisions_[state.texture] == texture.revision) {
            continue;
        }
        textureRevisions_[state.texture] = texture.revision;
        gl_.BindTexture(kGlTexture2D, textures_[state.texture]);
        gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), texture.image.width, texture.image.height, 0,
                       kGlRgba, kGlUnsignedByte, texture.image.rgba.data());
        gl_.GenerateMipmap(kGlTexture2D);
    }
}

void GlScene::uploadTextures(const kotor::Scene& scene, std::size_t first)
{
    if (first >= scene.textures.size()) {
        return;
    }
    textures_.resize(scene.textures.size());
    textureRevisions_.resize(scene.textures.size(), 0u);
    for (std::size_t i = first; i < scene.textures.size(); ++i) {
        textureRevisions_[i] = scene.textures[i].revision;
    }
    gl_.GenTextures(static_cast<GlSizei>(textures_.size() - first), textures_.data() + first);
    gl_.PixelStorei(kGlUnpackAlignment, 1);
    for (std::size_t i = first; i < scene.textures.size(); ++i) {
        const kotor::Image& image = scene.textures[i].image;
        gl_.BindTexture(kGlTexture2D, textures_[i]);
        gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<GlInt>(kGlLinearMipmapLinear));
        gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<GlInt>(kGlLinear));
        // Retail repeats unless the texture's TXI clamps an axis (retail 00424980, 00424dd0).
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapS,
                          static_cast<GlInt>((scene.textures[i].clamp & 1u) != 0u ? kGlClampToEdge
                                                                                  : kGlRepeat));
        gl_.TexParameteri(kGlTexture2D, kGlTextureWrapT,
                          static_cast<GlInt>((scene.textures[i].clamp & 2u) != 0u ? kGlClampToEdge
                                                                                  : kGlRepeat));
        gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), image.width, image.height, 0,
                       kGlRgba, kGlUnsignedByte, image.rgba.data());
        gl_.GenerateMipmap(kGlTexture2D);
    }
}

bool GlScene::syncMeshes(const kotor::Scene& scene, std::string& error)
{
    if (scene_ != &scene || meshes_.empty() || meshes_.front().vertexCount != scene.vertices.size()
        || meshes_.front().indexCount != scene.indices.size() || textures_.size() > scene.textures.size()) {
        return upload(scene, error);
    }
    const std::size_t knownTextures = textures_.size();
    uploadTextures(scene, knownTextures);
    std::unordered_map<std::uint64_t, std::size_t> held;
    held.reserve(meshes_.size());
    for (std::size_t i = 1; i < meshes_.size(); ++i) {
        held.emplace(meshes_[i].uid, i);
    }
    std::vector<GlMesh> next;
    next.reserve(scene.entityMeshes.size() + 1u);
    next.push_back(std::move(meshes_.front()));
    meshes_.front() = GlMesh{};
    std::size_t reused = 0;
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        const auto found = held.find(entityMesh.uid);
        if (found != held.end() && meshes_[found->second].vbo != 0
            && meshes_[found->second].vertexCount == entityMesh.vertices.size()
            && meshes_[found->second].indexCount == entityMesh.indices.size()) {
            next.push_back(std::move(meshes_[found->second]));
            meshes_[found->second] = GlMesh{};
            next.back().batches = entityMesh.batches;
            next.back().entity = entityMesh.entity;
            ++reused;
        } else {
            next.push_back(uploadMesh(entityMesh.vertices, entityMesh.indices, entityMesh.batches,
                                      entityMesh.entity));
            next.back().uid = entityMesh.uid;
            next.back().vertexCount = entityMesh.vertices.size();
            next.back().indexCount = entityMesh.indices.size();
        }
        next.back().visible = entityMesh.visible;
        next.back().fx = entityMesh.fx;
    }
    for (GlMesh& mesh : meshes_) {
        deleteMesh(mesh);
    }
    meshes_ = std::move(next);
    gl_.BindVertexArray(0);
    logInfo(concat("scene sync: meshes reused ", reused, " uploaded ", scene.entityMeshes.size() - reused,
                   " textures uploaded ", scene.textures.size() - knownTextures));
    return true;
}

void GlScene::refreshSceneVertices(const kotor::Scene& scene, std::size_t first, std::size_t count)
{
    if (scene_ != &scene || meshes_.empty() || meshes_.front().vbo == 0
        || meshes_.front().vertexCount != scene.vertices.size() || first + count > scene.vertices.size() || count == 0) {
        return;
    }
    gl_.BindBuffer(kGlArrayBuffer, meshes_.front().vbo);
    gl_.BufferSubData(kGlArrayBuffer, static_cast<GlSizeiptr>(first * sizeof(kotor::SceneVertex)),
                      static_cast<GlSizeiptr>(count * sizeof(kotor::SceneVertex)), scene.vertices.data() + first);
    gl_.BindBuffer(kGlArrayBuffer, 0);
}

void GlScene::refreshEntityMeshes(const kotor::Scene& scene)
{
    std::size_t index = 1;
    for (const kotor::EntityMesh& entityMesh : scene.entityMeshes) {
        if (index >= meshes_.size()) {
            break;
        }
        GlMesh& mesh = meshes_[index++];
        if (mesh.entity != entityMesh.entity) {
            continue;
        }
        mesh.visible = entityMesh.visible;
        mesh.fx = entityMesh.fx;
        gl_.BindBuffer(kGlArrayBuffer, mesh.vbo);
        gl_.BufferData(kGlArrayBuffer,
                       static_cast<GlSizeiptr>(entityMesh.vertices.size() * sizeof(kotor::SceneVertex)),
                       entityMesh.vertices.data(), kGlDynamicDraw);
    }
    gl_.BindBuffer(kGlArrayBuffer, 0);
}

void GlScene::drawBatch(const GlMesh* mesh, const kotor::TextureBatch& batch,
                        const kotor::SampledLight& light)
{
    if (batch.texture >= textures_.size() || batch.indexCount == 0) {
        return;
    }
    // Diagnostics only, off unless the variable is set (GL backend): KOTOR_DEBUG_SKIP_TEXTURES (comma
    // list of substrings) leaves matching batches out, to find which texture draws a region of a
    // paired frame; KOTOR_DEBUG_LIST_TEXTURES logs each texture drawn once, with its batch state.
    if (scene_ != nullptr && batch.texture < scene_->textures.size()) {
        static const std::string skip = [] {
            const char* value = SDL_getenv("KOTOR_DEBUG_SKIP_TEXTURES");
            return value != nullptr ? std::string(value) : std::string{};
        }();
        static const bool list = SDL_getenv("KOTOR_DEBUG_LIST_TEXTURES") != nullptr;
        const std::string& name = scene_->textures[batch.texture].name;
        if (!skip.empty()) {
            std::size_t start = 0;
            while (start <= skip.size()) {
                const std::size_t end = std::min(skip.find(',', start), skip.size());
                const std::string part = skip.substr(start, end - start);
                if (!part.empty() && name.find(part) != std::string::npos) {
                    return;
                }
                start = end + 1;
            }
        }
        if (list) {
            static std::unordered_map<std::string, int> seen;
            const auto textureName = [&](std::uint32_t index) {
                return index < scene_->textures.size() ? scene_->textures[index].name : std::string("-");
            };
            const std::string key = name + "|" + textureName(batch.lightmap);
            if (seen[key]++ == 0) {
                logInfo(concat("debug texture ", name, " lightmap ", textureName(batch.lightmap), " env ",
                               textureName(batch.envmap), " bump ", textureName(batch.bumpmap), " blend ",
                               static_cast<int>(batch.blend), " alphaRef ", batch.alphaRef, " envMask ",
                               batch.envMask ? 1 : 0, " shade ", kotor::batchShadeMode(batch), " room ", batch.room));
            }
        }
    }
    const kotor::SampledLight used = kotor::materialLight(batch, batch.bakedLight ? batch.baked : light);
    gl_.Uniform3f(uniforms_.ambient, used.ambient.x, used.ambient.y, used.ambient.z);
    for (std::size_t i = 0; i < used.lights.size(); ++i) {
        const kotor::SampledLight::Light& one = used.lights[i];
        gl_.Uniform4f(uniforms_.lightPosition[i], one.position.x, one.position.y, one.position.z, one.quadratic);
        gl_.Uniform3f(uniforms_.lightColour[i], one.colour.x, one.colour.y, one.colour.z);
    }
    const kotor::BatchUv uv = kotor::batchUvAt(batch, scene_ != nullptr ? scene_->clock : 0.0f);
    gl_.Uniform4f(uniforms_.uv, uv.scaleU, uv.scaleV, uv.offsetU, uv.offsetV);
    const bool lightmap = batch.lightmap < textures_.size();
    const bool envmap = batch.envmap < textures_.size() && batch.envMask;
    gl_.Uniform4f(uniforms_.params, kotor::batchAlphaRef(batch, frameEye_), lightmap ? 1.0f : 0.0f,
                  !envmap ? 0.0f : batch.envOnlyAlpha >= 0.0f ? 2.0f + batch.envOnlyAlpha : batch.envReflectionUnlit ? 1.25f : 1.0f,
                  kotor::batchShadeMode(batch));
    gl_.ActiveTexture(kGlTexture0);
    gl_.BindTexture(kGlTexture2D, textures_[batch.texture]);
    if (lightmap) {
        gl_.ActiveTexture(kGlTexture1);
        gl_.BindTexture(kGlTexture2D, textures_[batch.lightmap]);
    }
    if (envmap) {
        gl_.ActiveTexture(kGlTexture2);
        gl_.BindTexture(kGlTexture2D, textures_[batch.envmap]);
    }
    if (batch.bumpmap < textures_.size()) {
        const kotor::BumpCell cell = kotor::bumpCellAt(batch, scene_ != nullptr ? scene_->clock : 0.0f);
        gl_.Uniform4f(uniforms_.bump, static_cast<float>(batch.bumpCycleX), static_cast<float>(batch.bumpCycleY),
                      static_cast<float>(cell.x), static_cast<float>(cell.y));
        gl_.ActiveTexture(kGlTexture3);
        gl_.BindTexture(kGlTexture2D, textures_[batch.bumpmap]);
    } else {
        gl_.Uniform4f(uniforms_.bump, 0.0f, 1.0f, 0.0f, 0.0f);
    }
    gl_.Uniform1f(uniforms_.bumpSpecular, batch.bumpmap < textures_.size() ? batch.bumpSpecular : 0.0f);
    gl_.ActiveTexture(kGlTexture0);
    if (batch.blend == kotor::BlendMode::Opaque) {
        gl_.Disable(kGlBlend);
    } else {
        gl_.Enable(kGlBlend);
        gl_.BlendFunc(kGlSrcAlpha, batch.blend == kotor::BlendMode::Additive ? kGlOne : kGlOneMinusSrcAlpha);
    }
    gl_.DepthMask(batch.writeDepth ? 1 : 0);
    if (batch.depthTest) {
        gl_.Enable(kGlDepthTest);
    } else {
        gl_.Disable(kGlDepthTest);
    }
    if (batch.twoSided) {
        gl_.Disable(kGlCullFace);
    } else {
        gl_.Enable(kGlCullFace);
    }
    const kotor::Vec3 fog = kotor::batchFogColour(batch, fogColor_);
    gl_.Uniform3f(uniforms_.fogColor, fog.x, fog.y, fog.z);
    gl_.Uniform1f(uniforms_.fogOn, fogForFrame_ && !batch.noFog ? 1.0f : 0.0f);
    if (mesh != nullptr) {
        gl_.DrawElements(kGlTriangles, static_cast<GlSizei>(batch.indexCount), kGlUnsignedInt,
                         reinterpret_cast<const void*>(static_cast<std::size_t>(batch.firstIndex) * sizeof(std::uint32_t)));
    } else {
        gl_.DrawArrays(kGlTriangles, static_cast<GlInt>(batch.firstIndex),
                       static_cast<GlSizei>(batch.indexCount));
    }
}

void GlScene::drawShadows(const SceneCamera& camera, const float* viewProjection)
{
    constexpr GlEnum kGlStencilTest = 0x0B90u;
    constexpr GlEnum kGlStencilBufferBit = 0x00000400u;
    constexpr GlEnum kGlAlways = 0x0207u;
    constexpr GlEnum kGlKeep = 0x1E00u;
    constexpr GlEnum kGlIncr = 0x1E02u;
    constexpr GlEnum kGlDecr = 0x1E03u;
    constexpr GlEnum kGlCw = 0x0900u;
    constexpr GlEnum kGlCcw = 0x0901u;
    constexpr GlEnum kGlTriangleFan = 0x0006u;
    if (shadowProgram_ == 0 || scene_ == nullptr || !effect_.shadows) {
        return;
    }
    kotor::ShadowView view;
    view.enabled = true;
    view.focus = {effect_.shadowFocus[0], effect_.shadowFocus[1], effect_.shadowFocus[2]};
    view.eye = camera.eye;
    view.forward = camera.forward;
    view.farClip = camera.farClip;
    const kotor::ShadowFrame frame = kotor::buildShadowFrame(*scene_, view);
    if (!frame.active()) {
        return;
    }
    std::vector<float> data;
    data.reserve((frame.volume.size() + frame.disk.size()) * 4u);
    for (const kotor::Vec3& point : frame.volume) {
        data.insert(data.end(), {point.x, point.y, point.z, 0.0f});
    }
    for (std::size_t i = 0; i < frame.disk.size(); ++i) {
        const kotor::Vec3& point = frame.disk[i];
        data.insert(data.end(), {point.x, point.y, point.z, i == 0 ? frame.diskAlpha : 0.0f});
    }
    gl_.UseProgram(shadowProgram_);
    gl_.UniformMatrix4fv(shadowViewProjection_, 1, 0, viewProjection);
    gl_.Uniform4f(shadowFog_, fogColor_.x, fogColor_.y, fogColor_.z, fogForFrame_ ? 1.0f : 0.0f);
    gl_.Uniform4f(shadowEye_, camera.eye.x, camera.eye.y, camera.eye.z, fogNear_);
    gl_.Uniform1f(shadowFogFar_, fogFar_);
    gl_.BindVertexArray(shadowVao_);
    gl_.BindBuffer(kGlArrayBuffer, shadowVbo_);
    gl_.BufferData(kGlArrayBuffer, static_cast<GlSizeiptr>(data.size() * sizeof(float)), data.data(), kGlDynamicDraw);
    // Scene::Render clears the stencil to 2^(bits-1)-1 before RenderShadows (0045172e).
    gl_.StencilMask(0xFFu);
    gl_.ClearStencil(0x7F);
    gl_.Clear(kGlStencilBufferBit);
    // 00451a50: stencil test on, depth and colour writes off, glStencilFunc(GL_ALWAYS, 0xf, 0xf); the
    // depth test stays on. Each part (00426660): GL_CW with depth-fail GL_INCR, then GL_CCW with GL_DECR.
    // 00426660's two-sided z-pass branch needs 007a68dc, which no code writes, so retail always takes
    // this one; the cull face it relies on stays on from the static geometry.
    gl_.Enable(kGlStencilTest);
    gl_.DepthMask(0);
    gl_.ColorMask(0, 0, 0, 0);
    gl_.StencilFunc(kGlAlways, 0x0F, 0x0Fu);
    gl_.Disable(kGlBlend);
    gl_.Enable(kGlDepthTest);
    gl_.Enable(kGlCullFace);
    gl_.CullFace(kGlBack);
    for (const kotor::ShadowFrame::Run& run : frame.runs) {
        gl_.FrontFace(kGlCw);
        gl_.StencilOp(kGlKeep, kGlIncr, kGlKeep);
        gl_.DrawArrays(kGlTriangles, static_cast<GlInt>(run.first), static_cast<GlSizei>(run.count));
        gl_.FrontFace(kGlCcw);
        gl_.StencilOp(kGlKeep, kGlDecr, kGlKeep);
        gl_.DrawArrays(kGlTriangles, static_cast<GlInt>(run.first), static_cast<GlSizei>(run.count));
    }
    gl_.ColorMask(1, 1, 1, 1);
    gl_.StencilMask(0u);
    // 00450730: glStencilFunc(GL_LEQUAL, 1 << (bits - 1), ~0), GL_KEEP everywhere, depth test off,
    // SRC_ALPHA / ONE_MINUS_SRC_ALPHA, the fan.
    gl_.StencilFunc(kGlLequal, 0x80, 0xFFFFFFFFu);
    gl_.StencilOp(kGlKeep, kGlKeep, kGlKeep);
    gl_.Disable(kGlDepthTest);
    gl_.Enable(kGlBlend);
    gl_.BlendFunc(kGlSrcAlpha, kGlOneMinusSrcAlpha);
    gl_.DrawArrays(kGlTriangleFan, static_cast<GlInt>(frame.volume.size()), static_cast<GlSizei>(frame.disk.size()));
    gl_.Enable(kGlDepthTest);
    gl_.StencilMask(0xFFu);
    gl_.Disable(kGlStencilTest);
    gl_.DepthMask(1);
    gl_.Disable(kGlBlend);
}

std::size_t GlScene::drawMeshes(const SceneCamera& camera, float aspect, const PreviewView* view)
{
    gl_.UseProgram(program_);
    const Mat4 viewMatrix = lookAt(camera.eye, camera.forward, camera.up);
    const Mat4 projection = perspective(camera.fovYDegrees, aspect, camera.nearClip, camera.farClip);
    const Mat4 viewProjection = multiply(projection, viewMatrix);
    gl_.UniformMatrix4fv(uniforms_.viewProjection, 1, 0, viewProjection.m);
    gl_.Uniform3f(uniforms_.eye, camera.eye.x, camera.eye.y, camera.eye.z);
    frameEye_ = camera.eye;
    gl_.Uniform1f(uniforms_.fogNear, fogNear_);
    gl_.Uniform1f(uniforms_.fogFar, fogFar_);
    fogForFrame_ = fogOn_ && view == nullptr;
    gl_.Uniform1i(uniforms_.texture, 0);
    gl_.Uniform1i(uniforms_.lightmap, 1);
    gl_.Uniform1i(uniforms_.envMap, 2);
    gl_.Uniform1i(uniforms_.bumpMap, 3);

    // Retail's depth test and writes stay on through the default blend; additive draws without
    // writing (0047af70). Solid surfaces first, then everything that blends, far to near.
    struct Pending {
        const GlMesh* mesh;
        const kotor::TextureBatch* batch;
        Mat4 model;
        kotor::SampledLight light;
        float distance;
    };
    std::vector<Pending> blended;
    std::size_t drawnMeshes = 0;
    const std::vector<std::uint8_t>* rooms = scene_ != nullptr && view == nullptr && !scene_->visibleRooms.empty()
        ? &scene_->visibleRooms : nullptr;
    for (const GlMesh& mesh : meshes_) {
        if (!mesh.visible) {
            continue;
        }
        // ProgFX 1800: a flashing model leaves the normal buckets (ManageSceneBSP 0046ea32) for the
        // distortion list, drawn in applyDistortion.
        if (view == nullptr && !mesh.fx && mesh.entity != UINT32_MAX && effect_.hides(mesh.entity)) {
            continue;
        }
        Mat4 model = identity();
        kotor::SampledLight light;
        kotor::Vec3 meshCenter{};
        if (mesh.entity == UINT32_MAX) {
            if (view != nullptr && !view->drawStatic) {
                continue;
            }
            if (scene_ != nullptr) {
                light = kotor::sampleSceneLight(*scene_, camera.eye);
            }
        } else {
            if (view != nullptr
                && std::find(view->entities.begin(), view->entities.end(), mesh.entity) == view->entities.end()) {
                continue;
            }
            if (scene_ != nullptr && mesh.entity < scene_->entities.size()) {
                const kotor::Entity& entity = scene_->entities[mesh.entity];
                if (entity.dead && !entity.corpse()) {
                    continue;
                }
                model = multiply(translation(entity.position), rotationZ(entity.drawBearing()));
                meshCenter = entity.position + entity.lightOffset;
                light = kotor::objectSceneLight(*scene_, mesh.entity, meshCenter);
            }
        }
        gl_.UniformMatrix4fv(uniforms_.model, 1, 0, model.m);
        gl_.BindVertexArray(mesh.vao);
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.room >= 0 && rooms != nullptr && static_cast<std::size_t>(batch.room) < rooms->size()
                && (*rooms)[static_cast<std::size_t>(batch.room)] == 0u) {
                continue;
            }
            if (batch.maxDistance > 0.0f && kotor::length(batch.center - camera.eye) > batch.maxDistance) {
                continue;
            }
            if (batch.blend != kotor::BlendMode::Opaque
                && !(view == nullptr && mesh.entity == UINT32_MAX && kotor::drawsWithStaticGeometry(batch))) {
                const kotor::Vec3 center = mesh.entity == UINT32_MAX ? batch.center : meshCenter;
                blended.push_back({&mesh, &batch, model, light, kotor::length(center - camera.eye)});
                continue;
            }
            drawBatch(&mesh, batch, light);
        }
        ++drawnMeshes;
        // Scene::Render: RenderStaticGeometry, then RenderShadows, then the dynamic geometry.
        if (view == nullptr && &mesh == &meshes_.front()) {
            drawShadows(camera, viewProjection.m);
            gl_.UseProgram(program_);
        }
    }
    std::stable_sort(blended.begin(), blended.end(), [](const Pending& a, const Pending& b) {
        return kotor::blendedDrawsBefore(*a.batch, a.distance, *b.batch, b.distance);
    });
    for (const Pending& pending : blended) {
        gl_.UniformMatrix4fv(uniforms_.model, 1, 0, pending.model.m);
        gl_.BindVertexArray(pending.mesh->vao);
        drawBatch(pending.mesh, *pending.batch, pending.light);
    }
    // Particles: rebuilt per frame, already sorted far to near by emitter.
    if ((view == nullptr || view->drawParticles) && scene_ != nullptr && !scene_->particleMesh.vertices.empty()) {
        if (particleVao_ == 0) {
            gl_.GenVertexArrays(1, &particleVao_);
            gl_.GenBuffers(1, &particleVbo_);
            gl_.BindVertexArray(particleVao_);
            gl_.BindBuffer(kGlArrayBuffer, particleVbo_);
            const GlSizei stride = static_cast<GlSizei>(sizeof(kotor::SceneVertex));
            gl_.EnableVertexAttribArray(0);
            gl_.VertexAttribPointer(0, 3, kGlFloat, 0, stride,
                                    reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, position)));
            gl_.EnableVertexAttribArray(1);
            gl_.VertexAttribPointer(1, 3, kGlFloat, 0, stride,
                                    reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, normal)));
            gl_.EnableVertexAttribArray(2);
            gl_.VertexAttribPointer(2, 2, kGlFloat, 0, stride,
                                    reinterpret_cast<const void*>(offsetof(kotor::SceneVertex, uv)));
            bindExtraAttributes();
        }
        gl_.BindVertexArray(particleVao_);
        gl_.BindBuffer(kGlArrayBuffer, particleVbo_);
        gl_.BufferData(kGlArrayBuffer,
                       static_cast<GlSizeiptr>(scene_->particleMesh.vertices.size() * sizeof(kotor::SceneVertex)),
                       scene_->particleMesh.vertices.data(), kGlDynamicDraw);
        const Mat4 model = identity();
        gl_.UniformMatrix4fv(uniforms_.model, 1, 0, model.m);
        const kotor::SampledLight none;
        for (const kotor::TextureBatch& batch : scene_->particleMesh.batches) {
            drawBatch(nullptr, batch, none);
        }
    }
    gl_.Disable(kGlBlend);
    gl_.Disable(kGlCullFace);
    gl_.Enable(kGlDepthTest);
    gl_.DepthMask(1);
    gl_.BindVertexArray(0);
    return drawnMeshes;
}

void GlScene::render(const SceneCamera& camera, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    gl_.Viewport(0, 0, width, height);
    const kotor::Vec3 clear = fogOn_ ? fogColor_ : kotor::Vec3{};
    gl_.ClearColor(clear.x, clear.y, clear.z, 1.0f);
    gl_.ClearDepth(1.0);
    gl_.DepthMask(1);
    gl_.Clear(kGlColorBufferBit | kGlDepthBufferBit);
    if (program_ == 0 || meshes_.empty()) {
        return;
    }
    refreshProceduralTextures();
    gl_.Enable(kGlDepthTest);
    gl_.DepthFunc(kGlLequal);
    const std::size_t drawnMeshes = drawMeshes(camera, static_cast<float>(width) / static_cast<float>(height), nullptr);
    if (shadowProbeRequested_) {
        // The stencil as the volumes left it: no later pass writes it (stencil writes are masked off
        // after 00450730's fan).
        constexpr GlEnum kGlStencilIndex = 0x1901u;
        std::vector<std::uint8_t> stencil(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
        gl_.PixelStorei(kGlPackAlignment, 1);
        gl_.ReadPixels(0, 0, width, height, kGlStencilIndex, kGlUnsignedByte, stencil.data());
        shadowProbeCovered_ = static_cast<std::size_t>(
            std::count_if(stencil.begin(), stencil.end(), [](std::uint8_t value) { return value >= 0x80u; }));
        shadowProbeTotal_ = stencil.size();
        shadowProbeRequested_ = false;
        shadowProbeReady_ = true;
    }
    // FrameBufferModifications 00437520 order: the speed blur (0x10) before the distortion (0x20).
    applySpeedBlur(width, height);
    applyDistortion(camera, width, height);
    applyVideoEffect(width, height);
    if (!loggedFirstFrame_) {
        loggedFirstFrame_ = true;
        const GlEnum drawError = gl_.GetError();
        std::size_t alphaBatches = 0;
        std::size_t additiveBatches = 0;
        for (const GlMesh& mesh : meshes_) {
            for (const kotor::TextureBatch& batch : mesh.batches) {
                if (batch.blend == kotor::BlendMode::Alpha) {
                    ++alphaBatches;
                } else if (batch.blend == kotor::BlendMode::Additive) {
                    ++additiveBatches;
                }
            }
        }
        logInfo(concat("Scene frame: eye ", camera.eye.x, ", ", camera.eye.y, ", ", camera.eye.z,
                       " meshes ", drawnMeshes, "/", meshes_.size(), " textures ", textures_.size(),
                       " alpha batches ", alphaBatches, " additive batches ", additiveBatches,
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
    gl_.ReadBuffer(kGlBack);
    return writeBmp(path, width, height, pixels);
}

bool GlScene::uploadMovieFrame(int width, int height, const std::uint8_t* rgba,
                               GlUint& texture, std::string& error)
{
    if (width <= 0 || height <= 0 || rgba == nullptr) {
        error = "invalid movie frame";
        return false;
    }
    if (texture == 0) {
        gl_.GenTextures(1, &texture);
        if (texture == 0) {
            error = "glGenTextures failed for the movie frame";
            return false;
        }
    }
    gl_.BindTexture(kGlTexture2D, texture);
    gl_.PixelStorei(kGlUnpackAlignment, 1);
    gl_.TexImage2D(kGlTexture2D, 0, static_cast<GlInt>(kGlRgba8), width, height, 0,
                   kGlRgba, kGlUnsignedByte, rgba);
    gl_.TexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<GlInt>(kGlLinear));
    gl_.TexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<GlInt>(kGlLinear));
    gl_.BindTexture(kGlTexture2D, 0);
    return true;
}

void GlScene::destroyMovieFrame(GlUint& texture)
{
    if (texture != 0) {
        gl_.DeleteTextures(1, &texture);
        texture = 0;
    }
}

void GlScene::renderView(const PreviewView& view, int windowWidth, int windowHeight)
{
    if (program_ == 0 || windowWidth <= 0 || windowHeight <= 0 || view.width <= 0
        || view.height <= 0) {
        return;
    }
    // Scissor rectangle: the view rectangle clamped to the window (top-left origin).
    const int left = std::max(view.x, 0);
    const int top = std::max(view.y, 0);
    const int right = std::min(view.x + view.width, windowWidth);
    const int bottom = std::min(view.y + view.height, windowHeight);
    if (right <= left || bottom <= top) {
        return;
    }
    gl_.Enable(kGlScissorTest);
    // GL window coordinates have a bottom-left origin.
    gl_.Scissor(left, windowHeight - bottom, right - left, bottom - top);
    // The viewport keeps the full (unclamped) rectangle so a view that is partly
    // outside the window is cropped instead of squashed.
    gl_.Viewport(view.x, windowHeight - (view.y + view.height), view.width, view.height);
    gl_.ClearColor(view.background.x, view.background.y, view.background.z, 1.0f);
    gl_.ClearDepth(1.0);
    gl_.Clear(kGlColorBufferBit | kGlDepthBufferBit);

    if (!meshes_.empty()) {
        gl_.Enable(kGlDepthTest);
        gl_.DepthFunc(kGlLequal);
        drawMeshes(view.camera, static_cast<float>(view.width) / static_cast<float>(view.height), &view);
        gl_.UseProgram(0);
    }
    gl_.Disable(kGlScissorTest);
    gl_.Viewport(0, 0, windowWidth, windowHeight);
}

bool GlScene::readFramePixels(bool backBuffer, int width, int height,
                              std::vector<std::uint8_t>& rgbaTopLeft)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    std::vector<std::uint8_t> bottomUp(stride * static_cast<std::size_t>(height), 0u);
    gl_.ReadBuffer(backBuffer ? kGlBack : kGlFront);
    gl_.PixelStorei(kGlPackAlignment, 1);
    gl_.ReadPixels(0, 0, width, height, kGlRgba, kGlUnsignedByte, bottomUp.data());
    gl_.ReadBuffer(kGlBack);
    rgbaTopLeft.assign(bottomUp.size(), 0u);
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* source = bottomUp.data() + static_cast<std::size_t>(height - 1 - row) * stride;
        std::uint8_t* target = rgbaTopLeft.data() + static_cast<std::size_t>(row) * stride;
        std::memcpy(target, source, stride);
        for (std::size_t i = 3; i < stride; i += 4) {
            target[i] = 255u; // presented frames are opaque; the window alpha is undefined
        }
    }
    return true;
}

} // namespace ke
