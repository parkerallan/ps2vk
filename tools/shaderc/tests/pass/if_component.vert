#version 450

// if without else, a negated condition reused with ||, and a component write
// (c.w = 2.0) that spirv-opt turns into a value.
layout(location = 0) in vec4 inPos;

layout(location = 0) out vec4 vColor;

void main()
{
    vec4 c = inPos;
    bool flip = inPos.x > inPos.y;
    if (flip)
        c = c.yxzw;
    if (!flip || inPos.z == 1.0)
        c.w = 2.0;
    gl_Position = inPos;
    vColor = c;
}
