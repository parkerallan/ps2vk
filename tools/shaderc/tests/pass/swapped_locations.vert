#version 450

// Inputs declared out of location order: VU1 must still read them sorted
// by location (color = location 0 first, position = location 1 second).
layout(location = 1) in vec4 inPos;
layout(location = 0) in vec4 inColor;

layout(location = 2) out vec4 vColor;

void main()
{
    gl_Position = inPos;
    vColor = inColor;
}
