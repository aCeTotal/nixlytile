/* Tells placeholder from final answers. */
#include "nixlytile.h"
#include "client.h"

/* Unanswered configures fade anyway. */
#define ANSWER_TIMEOUT_NS 300000000ULL
/* Placeholders repaint within this. */
#define REPAINT_NS 150000000ULL

/* Applied commit acks the configure. */
static int
shows_answer(Client *c)
{
	struct nx_surface *s = client_surface(c);

	if (!(s->current.committed & NX_SURFACE_STATE_BUFFER))
		return 0;
#ifdef XWAYLAND
	if (client_is_x11(c))
		return s->current.width == c->morph_w &&
				s->current.height == c->morph_h;
#endif
	return (int32_t)(c->surface.xdg->current.configure_serial -
			c->morph_serial) >= 0;
}

void
answer_note(Client *c)
{
	uint64_t now = get_time_ns();

	if (!c->morph_from || !shows_answer(c))
		return;
	if (!c->morph_matched++) {
		c->morph_match_ns = now;
		return;
	}
	/* Placeholder answers repaint soon. */
	if (c->morph_matched == 2 && now - c->morph_match_ns < REPAINT_NS)
		c->morph_two_phase = 1;
}

/* Final content, not a placeholder. */
int
answer_settled(Client *c, uint64_t now)
{
	if (c->morph_answered)
		return 1;
	if (c->morph_matched > 1 || (c->morph_matched && !c->morph_two_phase))
		return c->morph_answered = 1;
	if (c->morph_matched && now - c->morph_match_ns >= REPAINT_NS) {
		c->morph_two_phase = 0;
		return c->morph_answered = 1;
	}
	if (now - c->morph_expect_ns >= ANSWER_TIMEOUT_NS)
		return c->morph_answered = 1;
	return 0;
}
