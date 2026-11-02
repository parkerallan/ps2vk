#version 450

// Constructors from a swizzle, components, constants and scalar arithmetic.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(inPos.xyz, 1.0);
    vColor = vec4(inColor.r, inColor.g, 0.0, inColor.a * 0.5);
}
