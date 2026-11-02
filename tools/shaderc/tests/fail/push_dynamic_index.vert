#version 450

// An array index that is itself a push constant: dynamic indexing is a later
// plan item, so it must be rejected clearly for now.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 offsets[4];
    int  which;
} pc;

void main()
{
    gl_Position = inPos + pc.offsets[pc.which];
}
