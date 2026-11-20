/* Clock popup: calendar, agenda, sync. */
#ifndef CALENDAR_H
#define CALENDAR_H

/* Include after nixlytile.h. */

typedef struct {
	char google_id[192];
	char google_secret[128];
	char microsoft_id[192];
} CalendarConfig;

extern CalendarConfig calendar_config;

void calendar_init(void);
/* Pointer rests on the clock. */
void calendar_opening(Monitor *m);
void calendar_render(Monitor *m);
void calendar_closed(Monitor *m);
void calendar_click(int id);
/* x is popup-relative. */
void calendar_scroll(int x, int steps);
int calendar_holds_open(void);
void calendar_entry_changed(void);

#endif
