#ifndef PLATFORM_INPUT_XCURSOR_THEME_SEARCH_H
#define PLATFORM_INPUT_XCURSOR_THEME_SEARCH_H

struct xcursor_images;

typedef void (*xcursor_load_callback)(struct xcursor_images *images,
		void *user_data);

// Includes inherited themes.
void xcursor_load_theme(const char *theme, int size,
		xcursor_load_callback load_callback, void *user_data);

#endif
