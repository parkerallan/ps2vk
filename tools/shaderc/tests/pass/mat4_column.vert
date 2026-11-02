#version 450

// One column of a matrix read as a vec4, with no multiply: pc.mvp[2] is push
// constant qword 2.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

void main()
{
    gl_Position = inPos + pc.mvp[2];
}
