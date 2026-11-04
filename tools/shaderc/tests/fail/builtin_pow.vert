#version 450

// pow needs a logarithm, which VU1 has no unit for.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = pow(inPos, vec4(2.0));
}
