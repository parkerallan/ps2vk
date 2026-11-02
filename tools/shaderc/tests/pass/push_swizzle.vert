#version 450

// Push constant members read through swizzles, a component and a constant
// array index: all compile-time field maps over the prologue-loaded qwords.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    vec4 offset;
    vec4 colors[2];
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(inPos.xy + pc.offset.yx, inPos.z, pc.offset.w);
    vColor = inColor * pc.colors[1].bgra + pc.colors[0].x;
}
