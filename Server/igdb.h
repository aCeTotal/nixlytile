/*
 * Nixly Media Server - IGDB API Client
 * Internet Game Database metadata for retro gaming ROMs
 */

#ifndef IGDB_H
#define IGDB_H

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

/* Map ConsoleType enum to IGDB platform ID */
int igdb_console_to_platform(int console_type);

#endif /* IGDB_H */
