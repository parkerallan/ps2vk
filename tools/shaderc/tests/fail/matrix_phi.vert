#version 450

// A matrix chosen by a branch would need 4 registers moved per path.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    mat4 a;
    mat4 b;
} pc;

void main()
{
    mat4 m;
    if (inPos.x > 0.0)
        m = pc.a;
    else
        m = pc.b;
    gl_Position = m * inPos;
}
