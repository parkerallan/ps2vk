#version 450

// The texture coordinate goes through the same field maps and arithmetic as
// any other value: swapped and scaled before it is written.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main()
{
    gl_Position = inPos;
    vColor = inColor;
    vUV = inUV.yx * 2.0;
}
