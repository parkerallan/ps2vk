#version 450

// A local struct and a local array indexed by constants: scalar replacement
// splits them into values, nothing stays in VU1 memory.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 vColor;

struct Light {
    vec4 dir;
    vec4 color;
};

void main()
{
    Light l;
    vec4 corners[2];

    l.dir = normalize(vec4(1.0, 2.0, 3.0, 0.0));
    l.color = inColor;
    corners[0] = inPos;
    corners[1] = inPos + l.dir;

    gl_Position = corners[1];
    vColor = l.color * max(dot(corners[0], l.dir), 0.0);
}
