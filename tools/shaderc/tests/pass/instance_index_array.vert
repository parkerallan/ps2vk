#version 450

// gl_InstanceIndex as an array index: the batch header's float is truncated
// into VI08.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 tints[4];
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos;
    vColor = pc.tints[gl_InstanceIndex];
}
