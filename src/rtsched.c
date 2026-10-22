#include "nixlytile.h"

#include <errno.h>
#include <sched.h>
#include <string.h>

/* Below PipeWire's data loop. */
#define COMPOSITOR_RT_PRIO 2

static int rt_suspended;

/* Children and threads start SCHED_OTHER. */
void
compositor_rt_on(void)
{
	struct sched_param sp = { .sched_priority = COMPOSITOR_RT_PRIO };

	if (compositor_rt_applied)
		return;
	if (sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &sp) != 0) {
		wlr_log(WLR_INFO, "Compositor RT priority failed: %s", strerror(errno));
		return;
	}
	compositor_rt_applied = 1;
	wlr_log(WLR_INFO, "Compositor RT priority set (SCHED_RR, prio=%d)",
		COMPOSITOR_RT_PRIO);
}

/* Commit-failure storms must not spin at RT. */
void
compositor_rt_suspend(void)
{
	struct sched_param sp = { .sched_priority = 0 };

	if (!compositor_rt_applied)
		return;
	sched_setscheduler(0, SCHED_OTHER | SCHED_RESET_ON_FORK, &sp);
	compositor_rt_applied = 0;
	rt_suspended = 1;
}

void
compositor_rt_resume(void)
{
	if (!rt_suspended)
		return;
	rt_suspended = 0;
	compositor_rt_on();
}
