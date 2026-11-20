#version 450

// Specialization constants: a bool, a float, an int, a vec4 with one
// specialized component and an operation on one at global scope. Each keeps
// its default here; the pipeline writes the values it is created with over
// them, so none may be folded away.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;

layout(constant_id = 0) const bool HALF = false;
layout(constant_id = 1) const float SCALE = 1.0;
layout(constant_id = 2) const int STEPS = 2;
const vec4 TINT = vec4(SCALE, 1.0, 0.0, 1.0);
const int TWICE = STEPS + STEPS;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = inPos * SCALE;
    float c = 0.0;
    for (int i = 0; i < TWICE; i++)
        c += 0.125;
    vColor = (HALF ? inColor.zyxw : inColor) * c * TINT;
}
