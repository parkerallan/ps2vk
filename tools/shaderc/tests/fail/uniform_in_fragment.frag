#version 450

layout(location = 0) in vec4 vColor;
layout(set = 0, binding = 0) uniform Material {
    vec4 tint;
} mat;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor * mat.tint;
}
