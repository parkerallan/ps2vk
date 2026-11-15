#version 450

// Integer division: VU1 has no integer ALU for it.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    int which;
} pc;

void main()
{
    gl_Position = inPos * float(pc.which / 2);
}
