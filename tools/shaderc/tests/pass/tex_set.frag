#version 450

// A sampler in a descriptor set other than 0: shaderc records set and binding.
layout(location = 1) in vec2 vUV;
layout(set = 1, binding = 2) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vUV);
}
