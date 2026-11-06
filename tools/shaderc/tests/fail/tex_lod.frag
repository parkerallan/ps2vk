#version 450

// The GS picks the mip level itself.
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = textureLod(tex, vUV, 0.0);
}
