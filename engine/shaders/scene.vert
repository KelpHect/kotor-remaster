#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProjection;
    vec4 ambient;
    vec4 diffuse;
    vec4 fogColor;
    vec2 fogNearFar;
    vec2 padding;
} pc;

void main() {
    vec4 world = pc.model * vec4(aPosition, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(pc.model) * aNormal;
    vTexCoord = aTexCoord;
    gl_Position = pc.viewProjection * world;
}
