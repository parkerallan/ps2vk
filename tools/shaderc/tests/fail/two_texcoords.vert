#version 450

// The GS interpolates one (S, T) pair.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inUV;

layout(location = 0) out vec2 vUV0;
layout(location = 1) out vec2 vUV1;

void main()
{
    gl_Position = inPos;
    vUV0 = inUV.xy;
    vUV1 = inUV.zw;
}
