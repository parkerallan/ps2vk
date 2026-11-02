#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

// Set per frame with pvkCmdPushConstants: where the triangle is this frame.
layout(push_constant) uniform Push {
    vec4 offset;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos + pc.offset;
    vColor = inColor;
}
