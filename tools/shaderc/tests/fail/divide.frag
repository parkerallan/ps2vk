#version 450

// Division per pixel is still out of reach for the GS.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor / 2.0;
}
