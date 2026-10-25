#version 450

// The GS interpolates only one color.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor0;
layout(location = 1) out vec4 vColor1;

void main()
{
    gl_Position = inPos;
    vColor0 = inColor;
    vColor1 = inColor;
}
