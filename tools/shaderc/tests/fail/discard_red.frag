#version 450

// The GS tests alpha only.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vColor.r < 0.5)
        discard;
    outColor = vColor;
}
