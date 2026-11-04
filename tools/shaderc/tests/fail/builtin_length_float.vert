#version 450

// length() of a float is abs(); the compiler asks for a vector.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = inPos * length(inPos.x);
}
