#version 450

// inPos.w is read in place as a broadcast operand: one MUL, no copy.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * inPos.w;
}
