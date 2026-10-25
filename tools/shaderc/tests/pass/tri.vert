#version 450

// Position and color passed straight through.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos;
    vColor = inColor;
}
