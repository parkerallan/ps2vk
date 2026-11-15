#version 450

// A local array indexed at run time stays in memory: spirv-opt can't promote it.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 offsets[4];
    int  which;
} pc;

void main()
{
    vec4 local[4] = pc.offsets;
    gl_Position = inPos + local[pc.which];
}
