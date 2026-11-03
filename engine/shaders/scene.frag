#version 450

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;

layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProjection;
    vec4 ambient;
    vec4 diffuse;
    vec4 fogColor;
    vec2 fogNearFar;
    vec2 padding;
} pc;

layout(set = 0, binding = 0) uniform sampler2D sceneTexture;

void main() {
    vec4 texel = texture(sceneTexture, vTexCoord);
    vec3 normal = normalize(vNormal);
    float light = max(dot(normal, normalize(vec3(-0.35, -0.55, 0.75))), 0.0);
    vec3 color = texel.rgb * (pc.ambient.rgb + pc.diffuse.rgb * light);
    float distanceToEye = length(vWorld);
    float fog = clamp((distanceToEye - pc.fogNearFar.x) / max(pc.fogNearFar.y - pc.fogNearFar.x, 1.0), 0.0, 1.0);
    color = mix(color, pc.fogColor.rgb, fog);
    fragColor = vec4(color, texel.a);
}
