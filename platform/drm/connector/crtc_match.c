#include <stdbool.h>
#include <string.h>
#include "platform/drm/connector/crtc_match.h"

static bool is_taken(size_t n, const uint32_t arr[static n], uint32_t key) {
	for (size_t i = 0; i < n; ++i) {
		if (arr[i] == key) {
			return true;
		}
	}
	return false;
}

struct match_state {
	const size_t num_conns;
	const uint32_t *restrict conns;
	const size_t num_crtcs;
	size_t score;
	size_t replaced;
	uint32_t *restrict res;
	uint32_t *restrict best;
	const uint32_t *restrict orig;
	bool exit_early;
};

/* Most matches, then fewest changes. */
static bool record_solution(struct match_state *st, size_t score, size_t replaced) {
	if (score < st->score || (score == st->score && replaced >= st->replaced)) {
		return false;
	}

	st->score = score;
	st->replaced = replaced;
	memcpy(st->best, st->res, sizeof(st->best[0]) * st->num_crtcs);

	st->exit_early = (st->score == st->num_crtcs
			|| st->score == st->num_conns)
			&& st->replaced == 0;
	return true;
}

/* Bipartite matching, preferring st->orig. */
static bool match_step(struct match_state *st,
		size_t score, size_t replaced, size_t crtc_index) {
	if (crtc_index >= st->num_crtcs) {
		return record_solution(st, score, replaced);
	}

	bool has_best = false;
	uint32_t orig = st->orig[crtc_index];

	if (orig != DRM_UNMATCHED && !is_taken(crtc_index, st->res, orig)) {
		st->res[crtc_index] = orig;
		size_t crtc_score = st->conns[orig] != 0 ? 1 : 0;
		has_best |= match_step(st, score + crtc_score, replaced, crtc_index + 1);
	}
	if (st->exit_early) {
		return true;
	}

	if (orig != DRM_UNMATCHED) {
		++replaced;
	}

	for (size_t candidate = 0; candidate < st->num_conns; ++candidate) {
		if (candidate == orig ||
				!(st->conns[candidate] & (1 << crtc_index)) ||
				is_taken(crtc_index, st->res, candidate)) {
			continue;
		}

		st->res[crtc_index] = candidate;
		size_t crtc_score = st->conns[candidate] != 0 ? 1 : 0;
		has_best |= match_step(st, score + crtc_score, replaced, crtc_index + 1);

		if (st->exit_early) {
			return true;
		}
	}

	// This CRTC may stay unmatched
	st->res[crtc_index] = DRM_UNMATCHED;
	has_best |= match_step(st, score, replaced, crtc_index + 1);

	return has_best;
}

void drm_match_connectors_with_crtcs(size_t num_conns,
		const uint32_t conns[static restrict num_conns],
		size_t num_crtcs, const uint32_t prev_crtcs[static restrict num_crtcs],
		uint32_t new_crtcs[static restrict num_crtcs]) {
	uint32_t solution[num_crtcs];
	for (size_t i = 0; i < num_crtcs; ++i) {
		solution[i] = DRM_UNMATCHED;
	}

	struct match_state st = {
		.num_conns = num_conns,
		.num_crtcs = num_crtcs,
		.score = 0,
		.replaced = SIZE_MAX,
		.conns = conns,
		.res = solution,
		.best = new_crtcs,
		.orig = prev_crtcs,
		.exit_early = false,
	};

	match_step(&st, 0, 0, 0);
}
