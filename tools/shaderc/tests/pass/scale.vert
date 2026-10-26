#version 450

// vec4 * float constant: one MULx with the constant broadcast from x.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * 0.5;
}
