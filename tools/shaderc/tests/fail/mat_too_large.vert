#version 450

// Three mat4s = 192 bytes: more than the 128 bytes ps2vk reserves. Checks that
// a matrix counts as 64 bytes.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 model;
    mat4 view;
    mat4 proj;
} pc;

void main()
{
    gl_Position = pc.proj * (pc.view * (pc.model * inPos));
}
