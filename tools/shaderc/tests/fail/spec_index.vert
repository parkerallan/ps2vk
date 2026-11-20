#version 450

// A specialization constant as an array index: the element's address is part
// of the code, so it cannot wait for the pipeline.
layout(location = 0) in vec4 inPos;

layout(constant_id = 0) const int WHICH = 0;

layout(push_constant) uniform Push {
    vec4 offsets[4];
} pc;

void main()
{
    gl_Position = inPos + pc.offsets[WHICH];
}
