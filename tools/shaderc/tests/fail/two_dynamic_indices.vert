#version 450

// A bone's column both picked at run time: one run-time index per read.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inBone;

layout(set = 0, binding = 0) uniform Skin {
    mat4 bones[4];
    int  column;
} skin;

void main()
{
    gl_Position = inPos + skin.bones[int(inBone.x)][skin.column];
}
