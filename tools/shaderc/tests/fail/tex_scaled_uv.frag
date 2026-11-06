#version 450

// No per-pixel arithmetic on the coordinate: scale it in the vertex shader.
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vUV * 2.0);
}
