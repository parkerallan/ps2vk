#version 450

// The same product written the other way round.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(binding = 2) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor * texture(tex, vUV);
}
