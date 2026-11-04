#version 450

// The elementary function unit: sin and cos reduce the angle to -pi..pi and
// run ESIN per field; exp and exp2 negate (and scale) for EEXP, which computes
// e^-x. A rotation by pc.time about z, with a sine wave added.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    float time;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(inPos.x * cos(pc.time) - inPos.y * sin(pc.time),
                       inPos.x * sin(pc.time) + inPos.y * cos(pc.time),
                       inPos.z, inPos.w) + sin(inPos);
    vColor = exp(inColor * sin(pc.time)) + exp2(-inColor);
}
