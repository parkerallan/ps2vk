#version 450

// Reductions over a register's fields: dot folds the product into x with
// broadcast adds, length and normalize go through SQRT/RSQRT in Q, cross uses
// the OPMULA/OPMSUB pair. (No locals yet, so the normal is normalized twice.)
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;

layout(push_constant) uniform Push {
    vec4 light;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(cross(normalize(inNormal.xyz), pc.light.xyz), length(inPos.xy));
    vColor = vec4(dot(normalize(inNormal.xyz), pc.light.xyz)) * inPos;
}
