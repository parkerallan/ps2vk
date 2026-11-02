#version 450

// Two matrices multiplied one after the other: proj * (model * pos). The eight
// columns fill the whole 128-byte push range, and the first product is an
// ordinary vec4 temporary that feeds the second chain.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 proj;
} pc;

void main()
{
    gl_Position = pc.proj * (pc.model * inPos);
}
