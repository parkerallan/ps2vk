#version 450

// Places each vertex of the cut quad between its 4 corners, lifts it by a wave, and passes on a
// tinted color, a texture coordinate and a fog amount.
layout(quads, fractional_odd_spacing, cw) in;

layout(push_constant) uniform Push {
    mat4 mvp;
    float height;
} pc;

layout(location = 0) in vec4 tColor[];
layout(location = 1) patch in vec4 tint;

layout(location = 0) out vec4 fColor;
layout(location = 1) out vec2 fUV;
layout(location = 2) out float fFog;

void main()
{
    float u = gl_TessCoord.x, v = gl_TessCoord.y;
    vec4 low = gl_in[0].gl_Position * (1.0 - u) + gl_in[1].gl_Position * u;
    vec4 high = gl_in[3].gl_Position * (1.0 - u) + gl_in[2].gl_Position * u;
    vec4 p = low * (1.0 - v) + high * v;

    p.y += sin(u * 6.2831853) * pc.height;
    fColor = (tColor[0] * (1.0 - u) + tColor[1] * u) * tint;
    fUV = vec2(u, v);
    fFog = v;
    gl_Position = pc.mvp * p;
}
