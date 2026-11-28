#version 450

// Places each vertex of the cut triangle by its three weights and blends the corners' colors.
layout(triangles, equal_spacing, ccw) in;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 levels;
} pc;

layout(location = 0) in vec4 tColor[];
layout(location = 0) out vec4 fColor;

void main()
{
    vec4 p = gl_in[0].gl_Position * gl_TessCoord.x + gl_in[1].gl_Position * gl_TessCoord.y +
             gl_in[2].gl_Position * gl_TessCoord.z;

    fColor = tColor[0] * gl_TessCoord.x + tColor[1] * gl_TessCoord.y + tColor[2] * gl_TessCoord.z;
    gl_Position = pc.mvp * p;
}
