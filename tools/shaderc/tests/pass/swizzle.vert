#version 450

// Reordered swizzle, and a vec4 assembled from two inputs.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(inPos.xy, inColor.zw);
    vColor = inColor.bgra;
}
