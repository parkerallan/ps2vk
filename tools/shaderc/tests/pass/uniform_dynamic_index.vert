#version 450

// An array element picked by an int uniform member.
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
