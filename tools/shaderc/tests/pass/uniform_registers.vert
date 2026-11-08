#version 450

// More push constant and uniform qwords than VU1 has free registers (two
// mat4s, a vec3, a float array and a vec4): each is loaded right before its
// first use and its register freed after its last, instead of all being
// held for the whole batch.
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
    mat4 model;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = cam.viewProj * (pc.model * inPos);
    vColor = lights.color * (max(dot(inNormal.xyz, lights.direction), 0.0) * lights.intensity[1]);
}
