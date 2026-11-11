#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aTexCoord1;
layout(location = 4) in vec4 aTint;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out vec2 vTexCoord1;
layout(location = 4) out vec4 vTint;
// Lit colour of a mesh without a lightmap or bump map, per vertex (see main).
layout(location = 5) out vec3 vLit;

layout(push_constant) uniform PushConstants {
    vec4 modelRows[3];
    mat4 viewProjection;
    vec4 ambient;           // rgb, w = fog far
    vec4 lightPositions[3]; // xyz, w = quadratic attenuation (< 0: a direction)
    uint lightColours[4];   // RGB8 of colour / 4
    vec4 fogColor;          // rgb, w = fog on
    vec4 eye;               // xyz, w = fog near
    vec4 uv;                // scale u v, offset u v
    vec4 params;            // alpha reference, lightmap, environment map, unlit (3: force shell)
} pc;

void main() {
    // A force shell (params.w 3): retail program 0x797488 / 0x794600 moves each vertex out along its
    // model-space normal by env[12].x, which 00477830 and 0048d040 set to the model's +0x11c (0.02 from 006a1220).
    vec4 local = vec4(pc.params.w > 2.5 && pc.params.w < 3.5 ? aPosition + aNormal * 0.02 : aPosition, 1.0);
    vec4 world = vec4(dot(pc.modelRows[0], local), dot(pc.modelRows[1], local), dot(pc.modelRows[2], local), 1.0);
    vWorld = world.xyz;
    vNormal = vec3(dot(pc.modelRows[0].xyz, aNormal), dot(pc.modelRows[1].xyz, aNormal), dot(pc.modelRows[2].xyz, aNormal));
    vTexCoord = aTexCoord;
    vTexCoord1 = aTexCoord1;
    vTint = aTint;
    // Retail draws these meshes with fixed-function GL lighting: the lights 004a2a00 binds are
    // evaluated at each vertex with its own normal and the clamped colour is interpolated. The
    // light model is one sided (no GL_LIGHT_MODEL_TWO_SIDE anywhere in the exe), so a back face
    // keeps its front normal. Paired frame sw_m01aa_10_E: long lhr_wall01 trim strips without a
    // lightmap measure texel x self illumination in retail, lit only at their far ends.
    vLit = vec3(0.0);
    if (pc.params.w < 0.5 && pc.params.y < 0.5 && pc.lightColours[3] == 0u) {
        vec3 normal = normalize(vNormal);
        vec3 light = pc.ambient.rgb;
        for (int i = 0; i < 3; ++i) {
            vec3 toLight = pc.lightPositions[i].xyz;
            float attenuation = 1.0;
            if (pc.lightPositions[i].w >= 0.0) {
                toLight -= world.xyz;
                attenuation = 1.0 / (1.0 + pc.lightPositions[i].w * dot(toLight, toLight));
                toLight = normalize(toLight);
            }
            light += unpackUnorm4x8(pc.lightColours[i]).rgb * 4.0 * attenuation * max(dot(normal, toLight), 0.0);
        }
        vLit = min(light + aTint.rgb, vec3(1.0));
    }
    gl_Position = pc.viewProjection * world;
}
