#version 450

// Row-vector form: v * M. ps2vk compiles M * v only.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

void main()
{
    gl_Position = inPos * pc.mvp;
}
