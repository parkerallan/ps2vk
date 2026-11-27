#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

// The sky the water reflects. Set 1, binding 0.
layout(set = 1, binding = 0) uniform sampler2D sky;

layout(location = 0) out vec4 outColor;

// GS modulate: the reflected sky times the water's light.
void main()
{
    outColor = texture(sky, vUV) * vColor;
}
