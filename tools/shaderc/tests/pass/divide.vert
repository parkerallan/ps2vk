#version 450

// Division by a constant (glslang splats 2.0 with OpCompositeConstruct) and by
// another vector: VU1 divides one component at a time through Q.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos / 2.0;
    vColor = inColor / inPos;
}
