#version 450

// A matrix column picked by a value computed at run time.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 mvp;
    int  which;
} pc;

void main()
{
    gl_Position = inPos + pc.mvp[pc.which];
}
