#version 450

// One texture per draw.
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex0;
layout(binding = 1) uniform sampler2D tex1;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex0, vUV) * texture(tex1, vUV);
}
