#version 450

// row_major stores rows in the qwords, so a column broadcast chain can't use it.
layout(location = 0) in vec4 inPos;

layout(push_constant, row_major) uniform Push {
    mat4 mvp;
} pc;

void main()
{
    gl_Position = pc.mvp * inPos;
}
