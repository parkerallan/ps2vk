#version 450

// The core case: a model-view-projection matrix in the push constants. The four
// columns are qwords 0-3 of the push range; VU1 multiplies them by the
// position's x, y, z, w through the accumulator: MULAx, MADDAy, MADDAz, MADDw.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = pc.mvp * inPos;
    vColor = inColor;
}
