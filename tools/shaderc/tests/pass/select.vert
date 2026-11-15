#version 450

// ?: as a conditional move, and an output stored on two paths: both stores
// move into one register.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    float s = inPos.x < 0.0 ? 0.5 : 1.0;
    gl_Position = inPos;
    vColor = inColor * s;
    if (inPos.z != 0.0)
        vColor = vec4(1.0);
}
