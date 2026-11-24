#version 450

// The discard in the else branch, the constant on the left: keep alpha >= 0.5.
layout(location = 0) in vec2 vUV;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 c = texture(tex, vUV);
    if (0.5 <= c.a)
        outColor = c;
    else
        discard;
}
