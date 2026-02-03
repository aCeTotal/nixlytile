/*
 * Nixly Media Server - TMDB API Client
 * TheMovieDB metadata scraping
 */

#ifndef TMDB_H
#define TMDB_H

#include <stdint.h>

typedef struct {
    int tmdb_id;
    char *title;
    char *original_title;
    char *overview;
    char *poster_path;      /* TMDB poster path (e.g., /abc123.jpg) */
    char *backdrop_path;    /* TMDB backdrop path */
    char *release_date;     /* YYYY-MM-DD */
    int year;
    float rating;           /* Vote average 0-10 */
    int vote_count;
    char *genres;           /* Comma-separated genre names */
    int runtime;            /* Minutes */
} TmdbMovie;

typedef struct {
    int tmdb_id;
    char *name;
    char *original_name;
    char *overview;
    char *poster_path;
    char *backdrop_path;
    char *first_air_date;
    int year;
    float rating;
    int vote_count;
    char *genres;
    int number_of_seasons;
    int number_of_episodes;
    int episode_run_time;     /* Typical episode runtime in minutes */
    char *status;             /* "Returning Series", "Ended", "Canceled", etc. */
    char *next_episode_date;  /* YYYY-MM-DD of next episode, NULL if none */
} TmdbTvShow;

typedef struct {
    int episode_number;
    int season_number;
    char *name;
    char *overview;
    char *still_path;       /* Episode screenshot */
    char *air_date;
    float rating;
    int runtime;
} TmdbEpisode;

/* Initialize TMDB client with API key */
int tmdb_init(const char *api_key, const char *language);

/* Cleanup */
void tmdb_cleanup(void);

/* Search for movie by title and optional year */
TmdbMovie *tmdb_search_movie(const char *title, int year);

/* Search for TV show by title and optional year */
TmdbTvShow *tmdb_search_tvshow(const char *title, int year);

/* Extended search: separate API filter year from scoring year
 * api_filter_year: used for TMDB's first_air_date_year filter (0 = no filter)
 * wanted_year: used for scoring/selecting best match from results
 */
TmdbTvShow *tmdb_search_tvshow_ex(const char *title, int api_filter_year, int wanted_year);

/* Get episode details */
TmdbEpisode *tmdb_get_episode(int tv_id, int season, int episode);

/* Get show status (status + next_episode_date) - lightweight refresh */
TmdbTvShow *tmdb_get_show_status(int tv_id);

/* Download image to local cache, returns local path */
char *tmdb_download_image(const char *tmdb_path, const char *size, const char *cache_dir);

/* Free result structures */
void tmdb_free_movie(TmdbMovie *m);
void tmdb_free_tvshow(TmdbTvShow *t);
void tmdb_free_episode(TmdbEpisode *e);

/* Build full image URL */
/* Sizes: w92, w154, w185, w342, w500, w780, original (posters) */
/*        w300, w780, w1280, original (backdrops) */
char *tmdb_image_url(const char *path, const char *size);

#endif /* TMDB_H */
