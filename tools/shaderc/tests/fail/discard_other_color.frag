#version 450

// The GS alpha test sees the alpha of the color written, not of the texture alone.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    if (texture(tex, vUV).a < 0.5)
        discard;
    outColor = texture(tex, vUV) * vColor;
}
