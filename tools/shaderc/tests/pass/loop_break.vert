#version 450

// Clip planes read in a loop with a break: the merge block's phi takes the
// running minimum from the loop test and 0.0 from the break.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    vec4 planes[4];
    int  count;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    float d = 1.0;
    for (int i = 0; i < pc.count; i++) {
        float t = dot(pc.planes[i], inPos);
        if (t < 0.0) {
            d = 0.0;
            break;
        }
        d = min(d, t);
    }
    gl_Position = inPos;
    vColor = vec4(d);
}
