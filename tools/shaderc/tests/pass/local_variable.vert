#version 450

// A local: spirv-opt turns the store and load into one value.
layout(location = 0) in vec4 inPos;

void main()
{
    vec4 p = inPos * 2.0;
    gl_Position = p;
}
