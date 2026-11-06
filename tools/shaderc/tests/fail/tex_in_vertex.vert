#version 450

// VU1 cannot read textures.
layout(location = 0) in vec4 inPos;
layout(binding = 0) uniform sampler2D heightMap;

void main()
{
    gl_Position = inPos + texture(heightMap, inPos.xy);
}
