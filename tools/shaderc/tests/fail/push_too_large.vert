#version 450

// 9 vec4s = 144 bytes: more than the 128 bytes ps2vk reserves in VU1 memory.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 rows[9];
} pc;

void main()
{
    gl_Position = inPos + pc.rows[0];
}
