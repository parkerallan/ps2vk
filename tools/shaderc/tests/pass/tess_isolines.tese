#version 450

// Lines across a quad of 4 control points, drawn as their points; the color tells the patch's number.
layout(isolines, equal_spacing, point_mode) in;

layout(location = 0) out vec4 fColor;

void main()
{
    float u = gl_TessCoord.x, v = gl_TessCoord.y;
    vec4 low = gl_in[0].gl_Position * (1.0 - u) + gl_in[1].gl_Position * u;
    vec4 high = gl_in[3].gl_Position * (1.0 - u) + gl_in[2].gl_Position * u;

    fColor = vec4(float(gl_PrimitiveID) * 0.25, float(gl_PatchVerticesIn) * 0.25, gl_TessLevelOuter[1] * 0.125, 1.0);
    gl_Position = low * (1.0 - v) + high * v;
}
