#version 450

// An array index that is itself a uniform: dynamic indexing is a later plan
// item, so it must be rejected clearly for now.
layout(location = 0) in vec4 inPos;

layout(set = 0, binding = 0) uniform Palette {
    vec4 colors[4];
    int  which;
} pal;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos;
    vColor = pal.colors[pal.which];
}
