#version 450

// Constants stored straight to the outputs, shared constants loaded once,
// and a long expression whose temporaries reuse registers.
layout(location = 0) in vec4 inPos;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = ((inPos * 2.0 + inPos) * 2.0 - inPos * 3.0) * 0.25 + inPos * 0.75;
    vColor = vec4(1.0, 0.5, 0.5, 1.0);
}
