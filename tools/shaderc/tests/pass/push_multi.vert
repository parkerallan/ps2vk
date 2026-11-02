#version 450

// Members of mixed type at std430 offsets: scale at byte 16 (field x of qword 1),
// pad at 20, uv at 24 (fields z, w of qword 1: copied out because arithmetic
// expects a vec2 in x, y), tint at 32 (qword 2).
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    vec4  offset;
    float scale;
    float pad;
    vec2  uv;
    vec3  tint;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos * pc.scale + pc.offset + vec4(pc.uv, 0.0, 0.0);
    vColor = vec4(inColor.rgb * pc.tint, inColor.a);
}
