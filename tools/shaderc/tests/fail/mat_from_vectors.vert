#version 450

// A matrix built from input vectors at run time; only push constant and
// constant matrices are supported.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inCol0;
layout(location = 2) in vec4 inCol1;

void main()
{
    gl_Position = mat4(inCol0, inCol1, vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0)) * inPos;
}
