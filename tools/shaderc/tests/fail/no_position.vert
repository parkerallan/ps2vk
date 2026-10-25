#version 450

// A vertex shader must write gl_Position.
layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    vColor = inColor;
}
