#version 450

layout(std430, binding = 0) buffer Data {
    vec4 v[];
} data;

void main()
{
    gl_Position = data.v[0];
}
