#version 450

// The GS can't run arithmetic per pixel.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor * 0.5;
}
