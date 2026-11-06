#version 450

// Position, color and a texture coordinate passed through: the textured cube's
// vertex shader. The epilogue emits ST (S/w, T/w, 1/w), RGBAQ, XYZ2.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main()
{
    gl_Position = inPos;
    vColor = inColor;
    vUV = inUV.xy;
}
