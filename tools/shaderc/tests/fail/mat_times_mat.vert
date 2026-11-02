#version 450

// Matrix times matrix: 16 multiply-adds per vertex that the CPU can do once.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 proj;
} pc;

void main()
{
    gl_Position = pc.proj * pc.model * inPos;
}
