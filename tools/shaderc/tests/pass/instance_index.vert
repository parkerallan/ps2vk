#version 450

// float(gl_InstanceIndex): the batch header's instance number, read like a push constant qword.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * float(gl_InstanceIndex);
}
