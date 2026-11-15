#version 450

// Integer arithmetic on gl_InstanceIndex: the batch header holds it as a float,
// so VU1 doubles the float.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * float(gl_InstanceIndex * 2);
}
