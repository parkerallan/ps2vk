#version 450

// A constant matrix: each column is a vec4 constant, loaded once with LOI
// before the loop like any other constant. This one scales x by 2 and
// translates y by 0.5.
layout(location = 0) in vec4 inPos;

const mat4 scaleShift = mat4(
    2.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.5, 0.0, 1.0);

void main()
{
    gl_Position = scaleShift * inPos;
}
