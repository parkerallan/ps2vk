#version 450

// The fog color comes first in mix(): 0 is all fog, 1 is no fog.
layout(location = 0) in vec4 vColor;
layout(location = 1) in float vFog;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(mix(vColor.rgb, vec3(0.5, 0.6, 0.7), vFog), vColor.a);
}
