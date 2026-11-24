#version 450

// The GS fogs red, green and blue only: mixing all four components is not the fog mix.
layout(location = 0) in vec4 vColor;
layout(location = 1) in float vFog;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = mix(vec4(0.5, 0.6, 0.7, 1.0), vColor, vFog);
}
