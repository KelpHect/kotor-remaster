#version 450

// Video effect pass: colour matrix then additive scan noise. Same maths as the
// OpenGL program in GlScene.cpp and applyVideoEffectCpu (render/VideoEffect.cpp).
// The scene copy has v = 0 at the top in Vulkan, so v is already screen-down.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 noiseScale;
    vec4 noiseTranslate;
    vec4 flags; // x: colour matrix, y: scan noise
} pc;

layout(set = 0, binding = 0) uniform sampler2D sceneCopy;
layout(set = 0, binding = 1) uniform sampler2D noiseTex;

void main() {
    vec3 c = texture(sceneCopy, vUv).rgb;
    vec3 o = c;
    if (pc.flags.x > 0.5) {
        o = vec3(dot(pc.row0.xyz, c), dot(pc.row1.xyz, c), dot(pc.row2.xyz, c));
    }
    if (pc.flags.y > 0.5) {
        float t = vUv.y * 7.5;
        o += texture(noiseTex, vec2(0.0, t)).rgb * pc.noiseScale.rgb + pc.noiseTranslate.rgb;
    }
    fragColor = vec4(clamp(o, 0.0, 1.0), 1.0);
}
