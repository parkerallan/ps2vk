#version 450

// Per-field built-ins on vec4s: abs, min, max, clamp, mix, floor, ceil, trunc
// and fract. glslang splats the float arguments of clamp and mix into vec4s.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = floor(clamp(inPos, 0.0, 1.0)) +
                  ceil(mix(min(abs(inPos), inColor), max(abs(inPos), inColor), inColor.w));
    vColor = fract(inPos) - trunc(inColor);
}
