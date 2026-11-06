#version 450

// Texture times interpolated color: GS TFX modulate with Gouraud shading.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vUV) * vColor;
}
