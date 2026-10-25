#version 450

// Arithmetic in a vertex shader isn't compiled yet.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * 0.5;
}
