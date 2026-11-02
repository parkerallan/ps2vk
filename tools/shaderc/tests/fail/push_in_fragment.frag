#version 450

// The GS cannot read per-draw values: push constants are vertex-only.
layout(location = 0) in vec4 vColor;

layout(push_constant) uniform Push {
    vec4 tint;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor * pc.tint;
}
