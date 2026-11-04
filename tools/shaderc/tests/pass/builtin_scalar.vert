#version 450

// Scalar built-ins on values that sit in different fields of their registers:
// pc.lo, pc.hi and pc.t are the x, y and z of one push constant qword, inPos.w
// is read in place. Results stay in the first argument's field and read the
// others by broadcast; mix lines its second argument up first.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    float lo;
    float hi;
    float t;
    vec4 scale;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos * clamp(inPos.w, pc.lo, pc.hi);
    vColor = vec4(mix(pc.lo, pc.hi, pc.t), min(inColor.x, pc.scale.y), abs(pc.t), 1.0);
}
