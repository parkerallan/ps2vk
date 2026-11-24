#version 450

// Discard on the alpha of texture times color, the value that is written.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 c = texture(tex, vUV) * vColor;
    if (c.a < 0.5)
        discard;
    outColor = c;
}
