#version 450

layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

// The outer cube, drawn on screen: each face shows the texture the first pass rendered.
void main()
{
    outColor = texture(tex, vUV);
}
