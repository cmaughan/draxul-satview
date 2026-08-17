#version 450

layout(set = 0, binding = 0) uniform sampler2D hdr_scene;

layout(push_constant) uniform ToneMapParams
{
    vec4 tone_map;
} params;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_frag_color;

#include "tone_map_aces.glsl"

void main()
{
    vec4 hdr = texture(hdr_scene, in_uv);
    vec3 mapped = tone_map_aces(hdr.rgb, params.tone_map.x, params.tone_map.y);
    out_frag_color = vec4(mapped, clamp(hdr.a, 0.0, 1.0));
}
