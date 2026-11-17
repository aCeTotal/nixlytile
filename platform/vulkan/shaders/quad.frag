#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 out_color;

// Matches struct nx_vk_frag_quad_pcr_data.
layout(push_constant) uniform UBO {
	layout(offset = 48) vec4 color;
	vec4 corner_box;
	float corner_radius;
	float softness;
	float border;
} data;

#include "rounded_box.glsl"

void main() {
	out_color = data.color;
	if (data.corner_radius <= 0 && data.softness <= 0 && data.border <= 0) {
		return;
	}

	float d = rounded_box(gl_FragCoord.xy, data.corner_box, data.corner_radius);
	float coverage = data.softness > 0 ?
		1.0 - smoothstep(-data.softness, data.softness, d) : clamp(0.5 - d, 0.0, 1.0);
	if (data.border > 0) {
		coverage *= clamp(d + data.border + 0.5, 0.0, 1.0);
	}
	out_color *= coverage;
}
