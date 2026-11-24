#version 450

// Every pixel discarded: the GS alpha test never passes.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor;
    discard;
}
