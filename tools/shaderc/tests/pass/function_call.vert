#version 450

// Calls are inlined by spirv-opt; the parameters and result become values.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 vColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 tint;
} pc;

vec4 transform(mat4 m, vec4 p)
{
    return m * p;
}

void shade(vec4 base, out vec4 result)
{
    result = base * pc.tint;
}

void main()
{
    gl_Position = transform(pc.mvp, inPos);
    shade(inColor, vColor);
}
