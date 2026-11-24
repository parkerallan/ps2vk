#version 450

// An 'if' that does not discard is code per pixel.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    if (vColor.a < 0.5)
        outColor = vColor;
}
