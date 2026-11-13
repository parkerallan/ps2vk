#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inUV;

// Set per draw with pvkCmdPushConstants: projection * view * model.
layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

// One vertex shader for both passes: the inner cube's fragment shader reads
// vColor, the outer cube's reads vUV.
void main()
{
    gl_Position = pc.mvp * inPos;
    vColor = inColor;
    vUV = inUV.xy;
}
