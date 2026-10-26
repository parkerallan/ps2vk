#version 450

// Without spirv-opt, glslang keeps 'p' as a Function variable.
layout(location = 0) in vec4 inPos;

void main()
{
    vec4 p = inPos * 2.0;
    gl_Position = p;
}
