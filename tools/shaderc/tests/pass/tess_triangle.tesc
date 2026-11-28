#version 450

// Passes a triangle's 3 control points on with their colors and sets the levels from a push constant.
layout(vertices = 3) out;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 levels; // the three outer levels, then the inner one
} pc;

layout(location = 0) in vec4 vColor[];
layout(location = 0) out vec4 tColor[];

void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    tColor[gl_InvocationID] = vColor[gl_InvocationID];
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = pc.levels.x;
        gl_TessLevelOuter[1] = pc.levels.y;
        gl_TessLevelOuter[2] = pc.levels.z;
        gl_TessLevelInner[0] = pc.levels.w;
    }
}
