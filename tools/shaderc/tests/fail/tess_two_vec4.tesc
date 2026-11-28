#version 450

layout(vertices = 3) out;

layout(location = 0) in vec4 vColor[];
layout(location = 1) in vec4 vNormal[];
layout(location = 0) out vec4 tColor[];

void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    tColor[gl_InvocationID] = vColor[gl_InvocationID] + vNormal[gl_InvocationID];
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
