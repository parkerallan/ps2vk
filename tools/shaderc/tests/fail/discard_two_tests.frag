#version 450

// The GS has one alpha comparison per draw.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vColor.a < 0.2 || vColor.a > 0.8)
        discard;
    outColor = vColor;
}
