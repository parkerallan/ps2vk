#version 450

// Skinning: a bone matrix picked per vertex by int() of an attribute. The
// index is truncated with FTOI0, moved to VI08 and scaled to the 4 qwords of a
// mat4 with two adds; the matrix's columns are then loaded relative to VI08.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inBone;

layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
} cam;

layout(set = 0, binding = 1) uniform Skin {
    mat4 bones[8];
} skin;

void main()
{
    gl_Position = cam.viewProj * (skin.bones[int(inBone.x)] * inPos);
}
