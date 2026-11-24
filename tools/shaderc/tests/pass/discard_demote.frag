#version 450
#extension GL_EXT_demote_to_helper_invocation : require

// demote compiles to OpDemoteToHelperInvocation: the same GS alpha test as discard.
layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor;
    if (outColor.a < 0.3)
        demote;
}
