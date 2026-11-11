#version 450
// The fan of Scene::RenderStencilShadowsForThisLight 00450730: black at its vertex alpha, fogged
// as the scene's fragments. The volumes draw with colour writes off.
layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 fog;   // rgb, w = fog on
    vec4 eye;   // xyz, w = fog near
    vec4 range; // x = fog far
} pc;
layout(location = 0) in vec3 vWorld;
layout(location = 1) in float vAlpha;
layout(location = 0) out vec4 outColor;
void main()
{
    vec3 color = vec3(0.0);
    if (pc.fog.w > 0.5) {
        float fog = clamp((length(vWorld - pc.eye.xyz) - pc.eye.w) / max(pc.range.x - pc.eye.w, 1.0), 0.0, 1.0);
        color = mix(color, pc.fog.rgb, fog);
    }
    outColor = vec4(color, vAlpha);
}
