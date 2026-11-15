#version 450

// Skinning with a constant-count loop: spirv-opt unrolls it, so inWeights[i]
// and inBones[i] become constant components and the body is straight-line code.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inWeights;
layout(location = 2) in vec4 inBones;

layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
} cam;

layout(set = 0, binding = 1) uniform Skin {
    mat4 bones[8];
} skin;

layout(location = 0) out vec4 vColor;

void main()
{
    vec4 p = vec4(0.0);
    for (int i = 0; i < 4; i++)
        p += inWeights[i] * (skin.bones[int(inBones[i])] * inPos);
    gl_Position = cam.viewProj * p;
    vColor = inWeights;
}
