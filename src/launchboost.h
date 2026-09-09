/*
 * launchboost.h — full clocks for the first seconds of an app launch.
 *
 * On wall power there is nothing to do: the governor is already
 * performance and the clock cap is released, so a launch runs at max
 * anyway.  On battery powersave.c caps every policy at 60% of its range,
 * which is exactly the wrong moment to be slow — app start-up is a short
 * burst of single-threaded work where race-to-idle wins on both speed AND
 * energy.  A kick releases the cap for a couple of seconds and puts it
 * back; the powersave governor stays in charge the whole time, so the
 * cores still idle down between the bursts.
 *
 * All sysfs writes happen on a worker thread: cpuclock_cap() walks every
 * cpufreq policy, and doing that inline on the compositor thread is the
 * same mistake battwatch.c was written to undo.
 */
#ifndef LAUNCHBOOST_H
#define LAUNCHBOOST_H

/* Start the worker.  Call once from setup(). */
void launchboost_init(void);

/* Release the clock cap for LB_BOOST_MS.  Repeated kicks extend the
 * window rather than stacking.  No-op on wall power. */
void launchboost_kick(void);

#endif /* LAUNCHBOOST_H */
