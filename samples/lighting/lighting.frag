#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vRamp;

// A 256x8 ramp: ambient + max(dot, 0), indexed by the interpolated dot product.
layout(set = 0, binding = 2) uniform sampler2D ramp;

layout(location = 0) out vec4 outColor;

// GS modulate: the ramp's brightness times the material color.
void main()
{
    outColor = texture(ramp, vRamp) * vColor;
}
