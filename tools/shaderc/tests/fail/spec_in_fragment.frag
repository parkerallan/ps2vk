#version 450

// A specialization constant in a fragment shader: the GS has nothing to set.
layout(location = 0) in vec4 vColor;

layout(constant_id = 0) const float SCALE = 1.0;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor * SCALE;
}
