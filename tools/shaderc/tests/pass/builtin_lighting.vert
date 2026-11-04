#version 450

// Built-ins combined with matrix products and swizzles: per-vertex diffuse
// lighting, the way the lighting sample will do it.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 m;
    vec4 light;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = pc.m * inPos;
    vColor = inColor * max(dot(normalize((pc.m * inNormal).xyz), normalize(pc.light.xyz)), 0.0);
}
