#version 450

// A built-in the compiler knows but does not emit yet.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;

void main()
{
    gl_Position = vec4(reflect(inPos.xyz, inNormal.xyz), 1.0);
}
