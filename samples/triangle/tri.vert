#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

// Set per frame with pvkCmdPushConstants: where the triangle is this frame,
// as a translation in a column-major mat4 (four columns of 16 bytes).
layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = pc.mvp * inPos;
    vColor = inColor;
}
