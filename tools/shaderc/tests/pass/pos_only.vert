#version 450

// No color output: VU1 code should write black.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos;
}
