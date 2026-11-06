#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inUV;

// Set per frame with pvkCmdPushConstants: projection * view * model of the
// spinning cube, column-major (four columns of 16 bytes).
layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 1) out vec2 vUV;

void main()
{
    gl_Position = pc.mvp * inPos;
    vUV = inUV.xy;
}
