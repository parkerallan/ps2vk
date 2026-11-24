#version 450

// Fog next to a texture coordinate: ST, RGBAQ, FOG, XYZ2 per vertex.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vFog;

void main()
{
    gl_Position = inPosition;
    vColor = inColor;
    vUV = inUV.xy;
    vFog = inUV.z;
}
