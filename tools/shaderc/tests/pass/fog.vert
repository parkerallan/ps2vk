#version 450

// A float output is the fog amount: VU1 clamps it and sends it in the GS FOG register.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vFog;

void main()
{
    gl_Position = inPosition;
    vColor = inColor;
    vFog = 1.0 - inPosition.z;
}
