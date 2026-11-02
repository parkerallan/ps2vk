#version 450

// A mat3 times a vec3 (a normal transform): three-instruction chain with .xyz
// masks. std430 gives each mat3 column a 16-byte stride, so the columns are
// still one qword each, and the vec4 after them starts at byte 48.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;

layout(push_constant) uniform Push {
    mat3 normalMatrix;
    vec4 offset;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos + pc.offset;
    vColor = vec4(pc.normalMatrix * inNormal.xyz, 1.0);
}
