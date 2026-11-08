#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;

// Set once: the camera. Set 0, binding 0.
layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
} cam;

// Rewritten every frame as the light orbits: the draw reads the buffer when
// the command buffer is submitted. Set 0, binding 1.
layout(set = 0, binding = 1) uniform Light {
    vec4 direction;  // towards the light, world space, normalized; w unused
    vec4 color;      // w = 1
} light;

// Per draw: the model rotation (a pure rotation, so it turns normals too)
// and the material color.
layout(push_constant) uniform Push {
    mat4 model;
    vec4 material;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vRamp;

// The lit color is not computed here: a per-vertex max(dot, 0) interpolates
// as a sawtooth along the day/night line. Instead the dot product itself goes
// out as a texture coordinate and the fragment shader looks it up in a ramp
// texture, which the GS samples per pixel. -1..1 maps to texels 1..255 so
// bilinear filtering never wraps around.
void main()
{
    gl_Position = cam.viewProj * (pc.model * inPos);
    vColor = pc.material * light.color;
    vRamp = vec2(dot((pc.model * inNormal).xyz, light.direction.xyz) * 0.49609375 + 0.5, 0.0);
}
