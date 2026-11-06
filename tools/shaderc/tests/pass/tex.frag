#version 450

// The texture as is: GS TFX decal, the vertex color is ignored.
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vUV);
}
