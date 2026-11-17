// Signed distance, radius clamped.
float rounded_box(vec2 p, vec4 box, float r) {
	r = min(r, min(box.z, box.w));
	vec2 q = abs(p - box.xy) - box.zw + r;
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
