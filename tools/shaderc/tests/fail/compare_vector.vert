#version 450

// lessThan() on vectors: VU1 tests one value at a time.
layout(location = 0) in vec4 inPos;

void main()
{
    gl_Position = all(lessThan(inPos, vec4(1.0))) ? inPos : vec4(0.0);
}
