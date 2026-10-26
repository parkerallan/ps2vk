#version 450

// +, -, * between inputs and a vec4 constant, and negation.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = (inPos + vec4(1.0, -2.0, 0.0, 0.0)) * inColor - inPos;
    vColor = -inColor;
}
