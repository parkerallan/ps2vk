#version 450

// if/else merging a vec4: && short-circuits through a bool phi (glslang does
// that for any operand that is more than a variable), which is kept as a float.
// Each path moves its value into the merged register before branching on.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    vec4 c;
    if (inPos.y > 0.0 && inColor.w >= 0.5)
        c = inColor * pc.tint;
    else
        c = inColor;
    gl_Position = pc.mvp * inPos;
    vColor = c;
}
