#version 450

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

// The inner cube, drawn into the texture: flat face colors.
void main()
{
    outColor = vColor;
}
