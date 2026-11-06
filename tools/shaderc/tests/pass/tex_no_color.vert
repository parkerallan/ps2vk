#version 450

// Texture coordinate only: the color output is left black (the fragment shader
// is expected to use decal, which ignores it).
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inUV;

layout(location = 3) out vec2 vUV;

void main()
{
    gl_Position = inPos;
    vUV = inUV.xy;
}
