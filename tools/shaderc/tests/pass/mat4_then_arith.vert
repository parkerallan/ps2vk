#version 450

// The matrix product is a plain vec4 that the existing arithmetic consumes.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 offset;
} pc;

void main()
{
    gl_Position = pc.mvp * inPos + pc.offset;
}
