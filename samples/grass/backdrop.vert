#version 450

// The sky and the ground behind the grass: positions already in clip space.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos;
    vColor = inColor;
}
