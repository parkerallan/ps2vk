#version 450

// Two uniform blocks, declared out of order, and a push constant block. The
// blocks are placed in (set, binding) order: Lights (set 0) first, then
// Camera (set 1). std140 puts the vec3 and each float array element in a
// qword of its own.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;

layout(set = 1, binding = 0) uniform Camera {
    mat4 viewProj;
} cam;

layout(set = 0, binding = 2) uniform Lights {
    vec3  direction;
    float intensity[2];
    vec4  color;
} lights;

layout(push_constant) uniform Push {
    vec4 offset;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = cam.viewProj * (inPos + pc.offset);
    vColor = lights.color * (max(dot(inNormal.xyz, lights.direction), 0.0) * lights.intensity[1]);
}
