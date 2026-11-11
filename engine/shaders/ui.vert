#version 450
// UI quad vertex stage for the additive-blend pipeline (VulkanBackend). Same vertex layout and
// push constants as Dear ImGui's draw lists, so it runs under ImGui's pipeline layout.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(push_constant) uniform Push {
    vec2 scale;
    vec2 translate;
} pc;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
void main()
{
    vColor = aColor;
    vUV = aUV;
    gl_Position = vec4(aPos * pc.scale + pc.translate, 0.0, 1.0);
}
