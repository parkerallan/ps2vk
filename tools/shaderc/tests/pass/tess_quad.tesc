#version 450

// A quad of 4 control points. Every invocation writes its own point; behind the barrier()
// invocation 0 reads all four, sets each edge's level from the edge's length and writes a per-patch tint.
layout(vertices = 4) out;

layout(set = 0, binding = 0) uniform Detail {
    float perUnit;
} detail;

layout(location = 0) in vec4 vColor[];
layout(location = 0) out vec4 tColor[];
layout(location = 1) patch out vec4 tint;

void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    tColor[gl_InvocationID] = vColor[gl_InvocationID];
    barrier();
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = length(gl_out[0].gl_Position.xy - gl_out[3].gl_Position.xy) * detail.perUnit;
        gl_TessLevelOuter[1] = length(gl_out[0].gl_Position.xy - gl_out[1].gl_Position.xy) * detail.perUnit;
        gl_TessLevelOuter[2] = length(gl_out[1].gl_Position.xy - gl_out[2].gl_Position.xy) * detail.perUnit;
        gl_TessLevelOuter[3] = length(gl_out[3].gl_Position.xy - gl_out[2].gl_Position.xy) * detail.perUnit;
        gl_TessLevelInner[0] = gl_TessLevelOuter[1];
        gl_TessLevelInner[1] = gl_TessLevelOuter[0];
        tint = vec4(1.0, 0.9, 0.8, 1.0);
    }
}
