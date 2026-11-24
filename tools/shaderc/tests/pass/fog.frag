#version 450

// The color mixed toward a constant fog color by the float input: GS PRIM.FGE and FOGCOL.
layout(location = 0) in vec4 vColor;
layout(location = 1) in float vFog;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(mix(vec3(0.5, 0.6, 0.7), vColor.rgb, vFog), vColor.a);
}
