#version 450

// sqrt and inversesqrt: one field at a time through Q, like division. A scalar
// result is copied out of Q into x.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(sqrt(inColor.xy), inversesqrt(inPos.z), 1.0);
    vColor = inColor * inversesqrt(inPos.w);
}
