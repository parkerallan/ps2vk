#version 450

// A push constant float array is std430: 4 bytes per element, which an
// index in qwords cannot address.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    float scale[4];
    int   which;
} pc;

void main()
{
    gl_Position = inPos * pc.scale[pc.which];
}
