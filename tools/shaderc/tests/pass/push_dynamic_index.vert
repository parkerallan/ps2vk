#version 450

// An array element picked by an int push constant: ILW reads the int straight
// from VU1 memory into VI08 and the element is loaded relative to it.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 offsets[4];
    int  which;
} pc;

void main()
{
    gl_Position = inPos + pc.offsets[pc.which];
}
