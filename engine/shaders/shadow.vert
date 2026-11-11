#version 450
// Stencil shadow volumes and the darkening fan (VulkanScene::drawShadows). Same maths as the
// OpenGL shadow program in GlScene.cpp.
layout(location = 0) in vec4 aPoint; // xyz, w = alpha
layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 fog;   // rgb, w = fog on
    vec4 eye;   // xyz, w = fog near
    vec4 range; // x = fog far
} pc;
layout(location = 0) out vec3 vWorld;
layout(location = 1) out float vAlpha;
void main()
{
    vWorld = aPoint.xyz;
    vAlpha = aPoint.w;
    gl_Position = pc.viewProjection * vec4(aPoint.xyz, 1.0);
}
