#version 450
#extension GL_EXT_mesh_shader : require

layout(location = 0) perprimitiveEXT in vec4 pColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = pColor;
}
