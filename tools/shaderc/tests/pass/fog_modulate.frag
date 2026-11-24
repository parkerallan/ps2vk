#version 450

// Fog after texture times color, with a discard on the same color's alpha.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vFog;
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

const vec3 FOG = vec3(0.1, 0.1, 0.2);

void main()
{
    vec4 c = texture(tex, vUV) * vColor;
    if (c.a < 0.5)
        discard;
    outColor = vec4(mix(FOG, c.rgb, vFog), c.a);
}
