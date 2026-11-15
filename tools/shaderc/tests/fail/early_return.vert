#version 450

// A return in the middle of main: VU1 code has one exit.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos;
    if (inPos.w < 0.0)
        return;
    gl_Position = inPos * 2.0;
}
