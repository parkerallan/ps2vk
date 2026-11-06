#version 450

// The coordinate must be the vec2 input itself: the GS reads S and T from the
// vertex, not from the color.
layout(location = 0) in vec4 vColor;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, vColor.xy) * vColor;
}
