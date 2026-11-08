#version 450

// A uniform block in place of push constants: the mat4's columns are qwords 0-3
// of the uniform area, loaded into registers in the prologue like push constants.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
    vec4 tint;
} cam;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = cam.viewProj * inPos;
    vColor = inColor * cam.tint;
}
