#version 450

// Textures (a uniform sampler) aren't supported yet.
layout(location = 0) in vec4 vColor;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vColor.xy) * vColor;
}
