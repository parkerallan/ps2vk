#version 450

// A loop whose count is a push constant stays a loop: the counter is a float
// phi, the compare sets the MAC flags and IBNE branches back.
layout(location = 0) in vec4 inPos;

layout(push_constant) uniform Push {
    int  count;
    vec4 step;
} pc;

void main()
{
    vec4 p = inPos;
    int n = pc.count;
    while (n > 0) {
        p += pc.step;
        n--;
    }
    gl_Position = p;
}
