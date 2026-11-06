#version 450

// A vec3 output is neither a color (vec4) nor a texture coordinate (vec2).
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inUV;

layout(location = 0) out vec3 vUV;

void main()
{
    gl_Position = inPos;
    vUV = inUV.xyz;
}
