/*
 * Nixly Media Server - IGDB API Client
 * Internet Game Database metadata for retro gaming ROMs
 */

#ifndef IGDB_H
#define IGDB_H

#include <stdio.h>

typedef struct {
    int igdb_id;
    char *name;
    char *summary;
    int release_year;
    char *genres;           /* Comma-separated genre names */
    char *platforms;        /* Comma-separated platform names */
    char *developer;
    char *publisher;
    float rating;           /* 0-100 scale from IGDB */
    char *cover_url;        /* IGDB cover URL (t_cover_big size) */
} IgdbGame;

/* Initialize IGDB client with Twitch OAuth2 credentials */
int igdb_init(const char *client_id, const char *client_secret);

/* Cleanup */
void igdb_cleanup(void);

/* Check if IGDB client is initialized and available */
int igdb_is_available(void);

/* Search for a game by title, optionally filtered by IGDB platform ID
 * Returns NULL if not found or on error */
IgdbGame *igdb_search_game(const char *title, int platform_id);

/* Search by name pattern (case-insensitive contains) — fallback for when
 * full-text search fails.  Uses: where name ~ *"title"*  */
IgdbGame *igdb_search_game_by_name(const char *title, int platform_id);

/* Search by splitting title into individual words and requiring all to match.
 * Uses: where name ~ *"word1"* & name ~ *"word2"* ...
 * Catches compound word mismatches: "Duck Tales" finds "DuckTales" */
IgdbGame *igdb_search_game_by_words(const char *title, int platform_id);

/* Search by alternative game names in IGDB (different regional/marketing titles).
 * Uses alternative_names endpoint to find game IDs, then fetches game details. */
IgdbGame *igdb_search_by_alt_name(const char *title, int platform_id);

/* Fetch cover URL for a game by IGDB ID (targeted query, no search).
 * Returns cover URL string (caller frees) or NULL if not found */
char *igdb_get_game_cover(int igdb_id);

/* Free result */
void igdb_free_game(IgdbGame *g);

/* IGDB platform IDs for Nintendo consoles */
#define IGDB_PLATFORM_NES       18
#define IGDB_PLATFORM_SNES      19
#define IGDB_PLATFORM_N64       4
#define IGDB_PLATFORM_GAMECUBE  21
#define IGDB_PLATFORM_WII       5
#define IGDB_PLATFORM_GB        33
#define IGDB_PLATFORM_GBC       22
#define IGDB_PLATFORM_GBA       24
#define IGDB_PLATFORM_PS2       8
#define IGDB_PLATFORM_SWITCH    130
#define IGDB_PLATFORM_PS1       7
#define IGDB_PLATFORM_PS3       9
#define IGDB_PLATFORM_PS4       48
#define IGDB_PLATFORM_PSP       38
#define IGDB_PLATFORM_VITA      46
#define IGDB_PLATFORM_XBOX      11
#define IGDB_PLATFORM_XBOX360   12
#define IGDB_PLATFORM_WIIU      41
#define IGDB_PLATFORM_DS        20
#define IGDB_PLATFORM_3DS       37
#define IGDB_PLATFORM_GENESIS   29
#define IGDB_PLATFORM_SMS       64
#define IGDB_PLATFORM_SATURN    32
#define IGDB_PLATFORM_DREAMCAST 23
#define IGDB_PLATFORM_SEGACD    78
#define IGDB_PLATFORM_ATARI2600 59
#define IGDB_PLATFORM_TGFX16   86
#define IGDB_PLATFORM_32X       30
#define IGDB_PLATFORM_GAMEGEAR  35

/* Map ConsoleType enum to IGDB platform ID */
int igdb_console_to_platform(int console_type);

/* Set log file for API request/response tracking (NULL to disable) */
void igdb_set_log(FILE *f);

#endif /* IGDB_H */
