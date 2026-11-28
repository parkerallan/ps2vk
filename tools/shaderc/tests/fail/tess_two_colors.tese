#version 450

layout(triangles, equal_spacing, ccw) in;

layout(location = 0) out vec4 fColor;
layout(location = 1) out vec4 fNormal;

void main()
{
    fColor = vec4(gl_TessCoord, 1.0);
    fNormal = vec4(0.0, 0.0, 1.0, 0.0);
    gl_Position = gl_in[0].gl_Position * gl_TessCoord.x + gl_in[1].gl_Position * gl_TessCoord.y +
                  gl_in[2].gl_Position * gl_TessCoord.z;
}
