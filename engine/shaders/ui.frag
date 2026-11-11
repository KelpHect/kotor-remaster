#version 450
// UI quad fragment stage for the additive-blend pipeline: vertex colour times texel. Texture and
// sampler are separate descriptor sets, as in Dear ImGui's Vulkan backend (set 0 image, set 1
// sampler), so this runs under ImGui's pipeline layout.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform texture2D uTexture;
layout(set = 1, binding = 0) uniform sampler uSampler;
void main()
{
    fColor = vColor * texture(sampler2D(uTexture, uSampler), vUV);
}
