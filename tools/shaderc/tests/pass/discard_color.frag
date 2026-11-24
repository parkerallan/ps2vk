#version 450

// Discard on the color input's alpha: GS alpha test, keep alpha >= 0.3.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vColor.a < 0.3)
        discard;
    outColor = vColor;
}
