/*
 * Nixly Media Server - Database Module
 * SQLite-based media library database
 */

#ifndef DATABASE_H
#define DATABASE_H

#include <stdint.h>

typedef enum {
    MEDIA_TYPE_MOVIE = 0,
    MEDIA_TYPE_TVSHOW = 1,
    MEDIA_TYPE_EPISODE = 2
} MediaType;

typedef struct {
    int id;
    MediaType type;
    char *title;
    char *filepath;
    char *show_name;      /* For episodes: parent show name */
    int season;           /* For episodes */
    int episode;          /* For episodes */
    int64_t size;         /* File size in bytes */
    int duration;         /* Duration in seconds */
    int width;            /* Video width */
    int height;           /* Video height */
    char *codec_video;
    char *codec_audio;
    int64_t bitrate;
    char *added_date;
} MediaEntry;

/* Initialize database, create tables if needed */
int database_init(const char *db_path);

/* Close database connection */
void database_close(void);

/* Add or update media entry */
int database_add_media(MediaEntry *entry);

/* Check if file already exists in database */
int database_file_exists(const char *filepath);

/* Get total media count */
int database_get_count(void);

/* Get filepath by media ID */
char *database_get_filepath(int id);

/* JSON exports for API */
char *database_get_library_json(void);
char *database_get_movies_json(void);
char *database_get_tvshows_json(void);
char *database_get_media_json(int id);

/* Free a MediaEntry */
void database_free_entry(MediaEntry *entry);

#endif /* DATABASE_H */
