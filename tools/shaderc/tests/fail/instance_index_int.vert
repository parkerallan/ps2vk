#version 450

// Integer arithmetic on gl_InstanceIndex: VU1 only has it as a float.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * float(gl_InstanceIndex * 2);
}
