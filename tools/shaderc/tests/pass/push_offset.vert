#version 450

// One vec4 push constant added to the position: loaded once in the prologue,
// then read in place.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    vec4 offset;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos + pc.offset;
    vColor = inColor;
}
