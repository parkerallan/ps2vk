#version 450

layout(location = 0) in vec4 inPos;

// 65 qwords: one more than the VU1 uniform area holds.
layout(set = 0, binding = 0) uniform Big {
    vec4 v[65];
} big;

void main()
{
    gl_Position = inPos + big.v[64];
}
