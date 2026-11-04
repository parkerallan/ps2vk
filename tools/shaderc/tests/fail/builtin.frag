#version 450

// Built-in functions run on VU1, which the fragment stage has no equivalent of.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = abs(vColor);
}
