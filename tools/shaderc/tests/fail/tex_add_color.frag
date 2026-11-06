#version 450

// The GS has decal and modulate (and highlight), not texture + color.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vUV) + vColor;
}
