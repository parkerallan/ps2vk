#version 450

// A vector component picked at run time: VU1 addresses whole qwords only.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 scale;
    int  which;
} pc;

void main()
{
    gl_Position = inPos * pc.scale[pc.which];
}
