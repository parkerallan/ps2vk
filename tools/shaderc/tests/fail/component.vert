#version 450

// Reading one component is the swizzle feature, not arithmetic.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * inPos.w;
}
