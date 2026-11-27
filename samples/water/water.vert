#version 450

layout(location = 0) in vec4 inGrid;    // x, y: where the vertex sits on the surface; z, w: the sky texel it reflects when still
layout(location = 1) in vec4 inSurface; // the compute shader's height, light (0 to 1), slope across and slope along

layout(push_constant) uniform Push {
    layout(offset = 16) mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

// The reflection: a tilted surface reflects another part of the sky, so the sky texel
// moves with the slope, further along the view than across it; the light brightens or
// darkens it (GS modulate takes 0.5 as 1.0).
void main()
{
    vColor = vec4(inSurface.yyy, 1.0);
    vUV = inGrid.zw + vec2(inSurface.z * 0.12, inSurface.w * 0.35);
    gl_Position = pc.mvp * vec4(inGrid.x, inSurface.x * 0.045, inGrid.y, 1.0);
}
