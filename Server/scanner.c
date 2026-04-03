/*
 * Nixly Media Server - File Scanner Module
 * Scans directories for media files, extracts metadata using FFmpeg,
 * and fetches metadata from TMDB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <zip.h>
#include <archive.h>
#include <archive_entry.h>

#include "scanner.h"
#include "database.h"
#include "tmdb.h"
#include "igdb.h"
#include "config.h"

/* Two independent progress trackers — TMDB and IGDB run in parallel */
#define SCRAPE_PROGRESS_INIT { \
    .active = 0, .operation = "idle", .current_item = "", \
    .total = 0, .processed = 0, .success = 0, \
    .start_time = 0, .session_active = 0, \
    .lock = PTHREAD_MUTEX_INITIALIZER }

ScrapeProgress tmdb_progress = SCRAPE_PROGRESS_INIT;
ScrapeProgress igdb_progress = SCRAPE_PROGRESS_INIT;

void scanner_scrape_session_begin(ScrapeProgress *p) {
    pthread_mutex_lock(&p->lock);
    p->session_active = 1;
    p->active = 1;
    p->operation = "starting";
    p->total = 0;
    p->processed = 0;
    p->success = 0;
    p->current_item[0] = '\0';
    p->start_time = time(NULL);
    pthread_mutex_unlock(&p->lock);
}

void scanner_scrape_session_end(ScrapeProgress *p) {
    pthread_mutex_lock(&p->lock);
    p->session_active = 0;
    p->active = 0;
    p->operation = "idle";
    p->current_item[0] = '\0';
    pthread_mutex_unlock(&p->lock);
}

static void scrape_begin(ScrapeProgress *p, const char *op, int total) {
    pthread_mutex_lock(&p->lock);
    p->active = 1;
    p->operation = op;
    if (p->session_active) {
        p->total += total;
    } else {
        p->total = total;
        p->processed = 0;
        p->success = 0;
        p->start_time = time(NULL);
    }
    p->current_item[0] = '\0';
    pthread_mutex_unlock(&p->lock);
}

static void scrape_update(ScrapeProgress *p, const char *item, int ok) {
    pthread_mutex_lock(&p->lock);
    p->processed++;
    if (ok) p->success++;
    if (item) {
        strncpy(p->current_item, item, sizeof(p->current_item) - 1);
        p->current_item[sizeof(p->current_item) - 1] = '\0';
    }
    pthread_mutex_unlock(&p->lock);
}

static void scrape_end(ScrapeProgress *p) {
    pthread_mutex_lock(&p->lock);
    if (!p->session_active) {
        p->active = 0;
        p->operation = "idle";
        p->current_item[0] = '\0';
    }
    pthread_mutex_unlock(&p->lock);
}

void scanner_get_progress(ScrapeProgress *p, int *active, const char **operation,
    char *current_item, int current_item_size,
    int *total, int *processed, int *success, time_t *start_time)
{
    pthread_mutex_lock(&p->lock);
    *active = p->active;
    *operation = p->operation;
    *total = p->total;
    *processed = p->processed;
    *success = p->success;
    *start_time = p->start_time;
    strncpy(current_item, p->current_item, current_item_size - 1);
    current_item[current_item_size - 1] = '\0';
    pthread_mutex_unlock(&p->lock);
}

/* Supported video extensions */
static const char *video_extensions[] = {
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v", ".ts", ".m2ts",
    ".wmv", ".flv", ".mpg", ".mpeg", ".vob", ".ogv", NULL
};

int scanner_is_media_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;

    for (int i = 0; video_extensions[i]; i++) {
        if (strcasecmp(ext, video_extensions[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Try to extract year from title like "Movie Name (2020)" or "Movie Name 2020 1080p BluRay" */
static int extract_year_from_title(const char *title, char *clean_title, size_t clean_size) {
    int year = 0;
    strncpy(clean_title, title, clean_size - 1);
    clean_title[clean_size - 1] = '\0';

    /* Look for (YYYY) pattern first */
    char *paren = strrchr(clean_title, '(');
    if (paren && strlen(paren) >= 5) {
        int y = atoi(paren + 1);
        if (y >= 1900 && y <= 2100) {
            year = y;
            /* Remove year from title */
            while (paren > clean_title && *(paren - 1) == ' ') paren--;
            *paren = '\0';
            return year;
        }
    }

    /* Scene release pattern: Look for FIRST year followed by resolution/source keywords
     * e.g., "Dying Young 1991 1080p BluRay" -> title="Dying Young", year=1991 */
    char *p = clean_title;
    while (*p) {
        /* Check if we found a 4-digit number that could be a year */
        if (isdigit(p[0]) && isdigit(p[1]) && isdigit(p[2]) && isdigit(p[3])) {
            int y = atoi(p);
            if (y >= 1900 && y <= 2100) {
                /* Check what comes after - if it's resolution/source keywords, this is likely the year */
                char *after = p + 4;
                /* Skip space/dot */
                while (*after == ' ' || *after == '.') after++;

                /* Common scene release keywords that follow the year */
                int is_scene = 0;
                if (strncasecmp(after, "1080", 4) == 0 ||
                    strncasecmp(after, "2160", 4) == 0 ||
                    strncasecmp(after, "720", 3) == 0 ||
                    strncasecmp(after, "480", 3) == 0 ||
                    strncasecmp(after, "4K", 2) == 0 ||
                    strncasecmp(after, "BluRay", 6) == 0 ||
                    strncasecmp(after, "BDRip", 5) == 0 ||
                    strncasecmp(after, "WEB", 3) == 0 ||
                    strncasecmp(after, "HDTV", 4) == 0 ||
                    strncasecmp(after, "DVDRip", 6) == 0 ||
                    strncasecmp(after, "Remux", 5) == 0 ||
                    strncasecmp(after, "x264", 4) == 0 ||
                    strncasecmp(after, "x265", 4) == 0 ||
                    strncasecmp(after, "HEVC", 4) == 0 ||
                    strncasecmp(after, "HDR", 3) == 0 ||
                    *after == '\0') {
                    is_scene = 1;
                }

                if (is_scene) {
                    year = y;
                    /* Truncate title at the year */
                    while (p > clean_title && (*(p-1) == ' ' || *(p-1) == '.')) p--;
                    *p = '\0';
                    return year;
                }
            }
        }
        p++;
    }

    /* Fallback: try YYYY at end without parentheses */
    if (year == 0) {
        size_t len = strlen(clean_title);
        if (len >= 4) {
            int y = atoi(clean_title + len - 4);
            if (y >= 1900 && y <= 2100) {
                char *space = clean_title + len - 5;
                if (space >= clean_title && (*space == ' ' || *space == '.')) {
                    year = y;
                    *space = '\0';
                }
            }
        }
    }

    return year;
}

/* Extract year from show name like "Macgyver.1985" or "Show (2020)"
 * Returns year and modifies show_name to remove the year portion
 */
static int extract_year_from_show(char *show_name) {
    int year = 0;
    size_t len = strlen(show_name);

    /* Look for (YYYY) pattern first */
    char *paren = strrchr(show_name, '(');
    if (paren && strlen(paren) >= 5) {
        int y = atoi(paren + 1);
        if (y >= 1900 && y <= 2100) {
            year = y;
            while (paren > show_name && *(paren - 1) == ' ') paren--;
            *paren = '\0';
            return year;
        }
    }

    /* Look for .YYYY or space YYYY at end */
    if (len >= 4) {
        char *p = show_name + len - 4;
        int y = atoi(p);
        if (y >= 1900 && y <= 2100) {
            /* Check if preceded by space or dot */
            if (p > show_name && (*(p-1) == ' ' || *(p-1) == '.')) {
                year = y;
                *(p-1) = '\0';
            }
        }
    }

    return year;
}

/* Clean up show/movie title - convert separators to spaces, preserve special chars */
static void clean_title(char *title) {
    char *src = title;
    char *dst = title;
    int last_was_space = 0;

    while (*src) {
        /* Convert dots and underscores to spaces, but preserve other special chars */
        if (*src == '.' || *src == '_') {
            if (!last_was_space) {
                *dst++ = ' ';
                last_was_space = 1;
            }
        } else {
            *dst++ = *src;
            last_was_space = (*src == ' ');
        }
        src++;
    }
    *dst = '\0';

    /* Trim trailing spaces */
    size_t len = strlen(title);
    while (len > 0 && title[len-1] == ' ') {
        title[--len] = '\0';
    }

    /* Trim leading spaces */
    src = title;
    while (*src == ' ') src++;
    if (src != title) {
        memmove(title, src, strlen(src) + 1);
    }
}

/* Try to extract TV show info from directory structure
 * Looks for paths like: .../nixly_ready_media/TV/<ShowName>[.Year]/Season<N>/<file>
 * or: .../TV/<ShowName>/Season<N>/<file>
 */
static int parse_tv_info_from_path(const char *filepath, char *show_name, int *season, int *episode, int *year) {
    *year = 0;
    *season = 0;
    *episode = 0;

    /* Look for /TV/ component in path */
    const char *tv_marker = strstr(filepath, "/TV/");
    if (!tv_marker) return 0;

    const char *show_start = tv_marker + 4; /* skip "/TV/" */
    if (*show_start == '\0') return 0;

    /* Find the next slash after show name */
    const char *show_end = strchr(show_start, '/');
    if (!show_end) return 0;

    /* Extract show name */
    size_t slen = show_end - show_start;
    if (slen == 0 || slen >= 256) return 0;
    strncpy(show_name, show_start, slen);
    show_name[slen] = '\0';
    clean_title(show_name);
    *year = extract_year_from_show(show_name);

    /* Try to find Season<N> */
    const char *season_start = show_end + 1;
    if (strncasecmp(season_start, "Season", 6) == 0) {
        *season = atoi(season_start + 6);
        if (*season <= 0) *season = 1;
    }

    /* Try to extract episode number from filename
     * Common patterns: Episode 1, E01, Ep01, 01, or just sequential numbering */
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    char name[256];
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *ext = strrchr(name, '.');
    if (ext) *ext = '\0';

    /* Try "Episode N", "E##", "Ep##" patterns */
    char *p = name;
    while (*p) {
        if (strncasecmp(p, "Episode", 7) == 0 && (isdigit(p[7]) || p[7] == ' ')) {
            const char *num = p + 7;
            while (*num == ' ') num++;
            if (isdigit(*num)) { *episode = atoi(num); break; }
        }
        if ((p[0] == 'E' || p[0] == 'e') && (p[1] == 'p' || p[1] == 'P') && isdigit(p[2])) {
            *episode = atoi(p + 2); break;
        }
        if ((p[0] == 'E' || p[0] == 'e') && isdigit(p[1]) &&
            (p == name || !isalpha(*(p-1)))) {
            *episode = atoi(p + 1); break;
        }
        p++;
    }

    /* Fallback: try leading number in filename (e.g., "01 - Title.mkv") */
    if (*episode == 0 && isdigit(name[0])) {
        *episode = atoi(name);
    }

    /* Must have at least show name to be useful */
    return (show_name[0] != '\0') ? 1 : 0;
}

/* Try to parse TV show info from filename
 * Patterns: Show.Name.S01E02, Show Name - 1x02, Show.Name.1985.S01E02, etc.
 * Falls back to directory structure (e.g., /TV/ShowName/Season1/file.mkv)
 */
int scanner_parse_tv_info(const char *filename, char *show_name, int *season, int *episode, int *year) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    char name[256];
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    /* Remove extension */
    char *ext = strrchr(name, '.');
    if (ext) *ext = '\0';

    *year = 0;

    /* Try S01E02 pattern */
    char *s = name;
    while (*s) {
        if ((s[0] == 'S' || s[0] == 's') && isdigit(s[1])) {
            int sn = 0, ep = 0;
            if (sscanf(s, "S%dE%d", &sn, &ep) == 2 ||
                sscanf(s, "s%de%d", &sn, &ep) == 2) {
                *season = sn;
                *episode = ep;

                /* Extract show name (everything before SxxExx) */
                size_t len = s - name;
                if (len > 0) {
                    strncpy(show_name, name, len);
                    show_name[len] = '\0';
                    clean_title(show_name);
                    /* Extract year from show name (e.g., "Macgyver 1985") */
                    *year = extract_year_from_show(show_name);
                }
                return 1;
            }
        }
        /* Try 1x02 pattern */
        if (isdigit(s[0]) && s[1] == 'x' && isdigit(s[2])) {
            int sn = 0, ep = 0;
            if (sscanf(s, "%dx%d", &sn, &ep) == 2) {
                *season = sn;
                *episode = ep;

                size_t len = s - name;
                if (len > 0) {
                    strncpy(show_name, name, len);
                    show_name[len] = '\0';
                    clean_title(show_name);
                    *year = extract_year_from_show(show_name);
                }
                return 1;
            }
        }
        s++;
    }

    /* Fallback: try to extract info from directory structure
     * e.g., /nixly_ready_media/TV/Breaking Bad/Season 1/Episode 1.mkv */
    return parse_tv_info_from_path(filename, show_name, season, episode, year);
}

/* Extract clean title from filename */
static void extract_title(const char *filepath, char *title, size_t title_size) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    strncpy(title, base, title_size - 1);
    title[title_size - 1] = '\0';

    /* Remove extension */
    char *ext = strrchr(title, '.');
    if (ext) *ext = '\0';

    /* Clean up the title (preserves special chars like colons) */
    clean_title(title);
}

/* Generate search variations for a movie title */
static void generate_movie_variations(const char *name, char variations[][256], int *count, int year) {
    *count = 0;

    /* Original name */
    strncpy(variations[(*count)++], name, 255);
    variations[*count - 1][255] = '\0';

    /* Try removing year suffix if present in name */
    char temp[256];
    strncpy(temp, name, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    size_t len = strlen(temp);
    if (len > 5) {
        char *last_space = strrchr(temp, ' ');
        if (last_space && strlen(last_space + 1) == 4) {
            int potential_year = atoi(last_space + 1);
            if (potential_year >= 1900 && potential_year <= 2100) {
                *last_space = '\0';
                if (*count < 6) {
                    strncpy(variations[(*count)++], temp, 255);
                    variations[*count - 1][255] = '\0';
                }
            }
        }
    }

    /* Try without special characters */
    strncpy(temp, name, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    char *src = temp, *dst = temp;
    while (*src) {
        if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') ||
            (*src >= '0' && *src <= '9') || *src == ' ') {
            *dst++ = *src;
        } else if (*src == ':' || *src == '-' || *src == '\'') {
            if (dst > temp && *(dst-1) != ' ') *dst++ = ' ';
        }
        src++;
    }
    *dst = '\0';
    /* Collapse multiple spaces */
    src = dst = temp;
    int last_space = 0;
    while (*src) {
        if (*src == ' ') {
            if (!last_space) *dst++ = ' ';
            last_space = 1;
        } else {
            *dst++ = *src;
            last_space = 0;
        }
        src++;
    }
    *dst = '\0';
    /* Trim trailing space */
    len = strlen(temp);
    while (len > 0 && temp[len-1] == ' ') temp[--len] = '\0';

    if (strcmp(temp, name) != 0 && *count < 6) {
        strncpy(variations[(*count)++], temp, 255);
        variations[*count - 1][255] = '\0';
    }

    (void)year; /* May use in future */
}

/* Fetch TMDB metadata for a movie */
static int fetch_movie_metadata(int db_id, const char *title) {
    char clean_title[256];
    int year = extract_year_from_title(title, clean_title, sizeof(clean_title));

    printf("  TMDB search: \"%s\" (year: %d)\n", clean_title, year);

    /* Generate search variations */
    char variations[6][256];
    int var_count = 0;
    generate_movie_variations(clean_title, variations, &var_count, year);

    TmdbMovie *movie = NULL;

    /* Try each variation until we find a match */
    for (int i = 0; i < var_count && !movie; i++) {
        printf("  TMDB try %d: \"%s\" (year: %d)\n", i + 1, variations[i], year);
        /* First try with year filter */
        movie = tmdb_search_movie(variations[i], year);
        /* If no result with year, try without */
        if (!movie && year > 0) {
            printf("  TMDB try %d: \"%s\" (no year filter)\n", i + 1, variations[i]);
            movie = tmdb_search_movie(variations[i], 0);
        }
    }

    if (movie) {
        MediaEntry tmdb_data = {0};
        tmdb_data.tmdb_id = movie->tmdb_id;
        tmdb_data.tmdb_title = movie->title;
        tmdb_data.overview = movie->overview;
        tmdb_data.release_date = movie->release_date;
        tmdb_data.year = movie->year;
        tmdb_data.rating = movie->rating;
        tmdb_data.vote_count = movie->vote_count;
        tmdb_data.genres = movie->genres;

        /* Download images */
        if (movie->poster_path) {
            tmdb_data.poster_path = tmdb_download_image(movie->poster_path, "w500",
                                                        server_config.cache_dir);
        }
        if (movie->backdrop_path) {
            tmdb_data.backdrop_path = tmdb_download_image(movie->backdrop_path, "w1280",
                                                          server_config.cache_dir);
        }

        database_update_tmdb(db_id, &tmdb_data);

        printf("  TMDB: Found \"%s\" (%d) - %.1f/10\n",
               movie->title, movie->year, movie->rating);

        free(tmdb_data.poster_path);
        free(tmdb_data.backdrop_path);
        tmdb_free_movie(movie);
        return 1;
    } else {
        printf("  TMDB: No match found after %d attempts\n", var_count);
        return 0;
    }
}

/* Generate search variations for a show name */
static void generate_search_variations(const char *name, char variations[][256], int *count, int year) {
    *count = 0;

    /* Original name */
    strncpy(variations[(*count)++], name, 255);

    /* Without year suffix if present */
    char temp[256];
    strncpy(temp, name, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    /* Try removing common suffixes like "US", "UK", year in parens */
    size_t len = strlen(temp);
    if (len > 3 && strcmp(temp + len - 2, "US") == 0) {
        temp[len - 2] = '\0';
        while (len > 0 && temp[len-1] == ' ') temp[--len] = '\0';
        strncpy(variations[(*count)++], temp, 255);
    }

    /* Try removing year suffix (e.g., "MacGyver 1985" -> "MacGyver") */
    strncpy(temp, name, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    len = strlen(temp);
    if (len > 5) {
        /* Check if ends with 4-digit year (19xx or 20xx) */
        char *last_space = strrchr(temp, ' ');
        if (last_space && strlen(last_space + 1) == 4) {
            int potential_year = atoi(last_space + 1);
            if (potential_year >= 1900 && potential_year <= 2100) {
                *last_space = '\0';
                if (*count < 6)
                    strncpy(variations[(*count)++], temp, 255);
            }
        }
    }

    /* If year is known, add it in different formats */
    if (year > 0 && *count < 6) {
        snprintf(variations[*count], 255, "%s (%d)", name, year);
        (*count)++;
        snprintf(variations[*count], 255, "%s %d", name, year);
        (*count)++;
    }

    /* Try without special characters for difficult matches */
    strncpy(temp, name, sizeof(temp) - 1);
    char *src = temp, *dst = temp;
    while (*src) {
        if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') ||
            (*src >= '0' && *src <= '9') || *src == ' ') {
            *dst++ = *src;
        } else if (*src == ':' || *src == '-') {
            /* Replace colon/dash with space if not already followed by space */
            if (dst > temp && *(dst-1) != ' ') *dst++ = ' ';
        }
        src++;
    }
    *dst = '\0';
    /* Collapse multiple spaces */
    src = dst = temp;
    int last_space = 0;
    while (*src) {
        if (*src == ' ') {
            if (!last_space) *dst++ = ' ';
            last_space = 1;
        } else {
            *dst++ = *src;
            last_space = 0;
        }
        src++;
    }
    *dst = '\0';
    if (strcmp(temp, name) != 0 && *count < 6) {
        strncpy(variations[(*count)++], temp, 255);
    }
}

/* Fetch TMDB metadata for a TV episode */
static int fetch_episode_metadata(int db_id, const char *show_name, int season, int episode, int year) {
    printf("  TMDB search: \"%s\" S%02dE%02d (year: %d)\n", show_name, season, episode, year);

    /* Generate search variations */
    char variations[6][256];
    int var_count = 0;
    generate_search_variations(show_name, variations, &var_count, year);

    TmdbTvShow *show = NULL;

    /* Try each variation until we find a match */
    for (int i = 0; i < var_count && !show; i++) {
        printf("  TMDB try %d: \"%s\" (year: %d)\n", i + 1, variations[i], year);
        /* First try with year filter for precise matching */
        show = tmdb_search_tvshow(variations[i], year);
        /* If no result with year filter, try without API filter but still use year for scoring */
        if (!show && year > 0) {
            printf("  TMDB try %d: \"%s\" (no API filter, scoring by year %d)\n", i + 1, variations[i], year);
            show = tmdb_search_tvshow_ex(variations[i], 0, year);
        }
    }

    if (!show) {
        printf("  TMDB: Show not found after %d attempts\n", var_count);
        return 0;
    }

    MediaEntry tmdb_data = {0};
    tmdb_data.tmdb_show_id = show->tmdb_id;
    tmdb_data.tmdb_title = show->name;
    tmdb_data.overview = show->overview;
    tmdb_data.year = show->year;
    tmdb_data.rating = show->rating;
    tmdb_data.vote_count = show->vote_count;
    tmdb_data.genres = show->genres;
    tmdb_data.tmdb_total_seasons = show->number_of_seasons;
    tmdb_data.tmdb_total_episodes = show->number_of_episodes;
    tmdb_data.tmdb_episode_runtime = show->episode_run_time;
    tmdb_data.tmdb_status = show->status;
    tmdb_data.tmdb_next_episode = show->next_episode_date;

    /* Download show poster */
    if (show->poster_path) {
        tmdb_data.poster_path = tmdb_download_image(show->poster_path, "w500",
                                                    server_config.cache_dir);
    }
    if (show->backdrop_path) {
        tmdb_data.backdrop_path = tmdb_download_image(show->backdrop_path, "w1280",
                                                      server_config.cache_dir);
    }

    /* Get episode details */
    TmdbEpisode *ep = tmdb_get_episode(show->tmdb_id, season, episode);
    if (ep) {
        tmdb_data.tmdb_id = show->tmdb_id; /* Use show ID for episodes */
        tmdb_data.episode_title = ep->name;
        tmdb_data.episode_overview = ep->overview;

        /* Use episode rating instead of show rating */
        if (ep->rating > 0) {
            tmdb_data.rating = ep->rating;
        }

        /* Fallback: use individual episode runtime if show-level runtime is 0 */
        if (tmdb_data.tmdb_episode_runtime == 0 && ep->runtime > 0) {
            tmdb_data.tmdb_episode_runtime = ep->runtime;
        }

        if (ep->still_path) {
            tmdb_data.still_path = tmdb_download_image(ep->still_path, "w300",
                                                       server_config.cache_dir);
        }

        printf("  TMDB: Found \"%s\" - \"%s\"\n", show->name, ep->name);
    } else {
        tmdb_data.tmdb_id = show->tmdb_id;
        printf("  TMDB: Found show \"%s\", episode not found\n", show->name);
    }

    database_update_tmdb(db_id, &tmdb_data);

    if (ep) tmdb_free_episode(ep);
    free(tmdb_data.poster_path);
    free(tmdb_data.backdrop_path);
    free(tmdb_data.still_path);
    tmdb_free_tvshow(show);
    return 1;
}

/* Probe media file with FFmpeg and add to database */
int scanner_scan_file(const char *filepath, int fetch_tmdb) {
    /* Skip if already in database */
    if (database_file_exists(filepath)) {
        return 0;
    }

    AVFormatContext *fmt_ctx = NULL;
    int ret = avformat_open_input(&fmt_ctx, filepath, NULL, NULL);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "  Warning: Cannot open %s: %s\n", filepath, err);
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    MediaEntry entry = {0};
    entry.filepath = (char *)filepath;

    /* Get file size */
    struct stat st;
    if (stat(filepath, &st) == 0) {
        entry.size = st.st_size;
    }

    /* Duration */
    if (fmt_ctx->duration > 0) {
        entry.duration = fmt_ctx->duration / AV_TIME_BASE;
    }

    /* Bitrate */
    entry.bitrate = fmt_ctx->bit_rate;

    /* Find video and audio streams */
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !entry.codec_video) {
            entry.width = codecpar->width;
            entry.height = codecpar->height;
            const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
            if (codec) {
                entry.codec_video = (char *)codec->name;
            }
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !entry.codec_audio) {
            const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
            if (codec) {
                entry.codec_audio = (char *)codec->name;
            }
        }
    }

    /* Determine type and extract title */
    char title[256];
    char show_name[256] = {0};
    int season = 0, episode = 0, show_year = 0;

    if (scanner_parse_tv_info(filepath, show_name, &season, &episode, &show_year)) {
        entry.type = MEDIA_TYPE_EPISODE;
        entry.show_name = show_name;
        entry.season = season;
        entry.episode = episode;
        entry.year = show_year;
        snprintf(title, sizeof(title), "%s S%02dE%02d", show_name, season, episode);
        entry.title = title;
    } else {
        entry.type = MEDIA_TYPE_MOVIE;
        extract_title(filepath, title, sizeof(title));
        entry.title = title;
    }

    printf("  Added: %s (%dx%d, %d min)\n",
           entry.title, entry.width, entry.height, entry.duration / 60);

    int db_id = database_add_media(&entry);
    avformat_close_input(&fmt_ctx);

    /* Fetch TMDB metadata in background or immediately */
    if (fetch_tmdb && db_id > 0 && server_config.tmdb_api_key[0]) {
        if (entry.type == MEDIA_TYPE_MOVIE) {
            fetch_movie_metadata(db_id, title);
        } else if (entry.type == MEDIA_TYPE_EPISODE) {
            fetch_episode_metadata(db_id, show_name, season, episode, show_year);
        }
    }

    return db_id;
}

/* Recursively scan directory */
int scanner_scan_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        perror(path);
        return -1;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip hidden files and . .. */
        if (entry->d_name[0] == '.') continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            count += scanner_scan_directory(fullpath);
        }
        else if (S_ISREG(st.st_mode) && scanner_is_media_file(fullpath)) {
            if (scanner_scan_file(fullpath, 1) >= 0) {
                count++;
            }
        }
    }

    closedir(dir);
    return count;
}

/* Fetch TMDB metadata for entries missing it */
void scanner_fetch_missing_tmdb(void) {
    if (!server_config.tmdb_api_key[0]) {
        printf("TMDB API key not configured, skipping metadata fetch\n");
        return;
    }

    MediaEntry *entries = NULL;
    int count = 0;

    if (database_get_entries_without_tmdb(&entries, &count) != 0 || count == 0) {
        printf("All entries have TMDB metadata\n");
        return;
    }

    printf("Fetching TMDB metadata for %d entries...\n", count);
    scrape_begin(&tmdb_progress, "tmdb_fetch", count);

    for (int i = 0; i < count; i++) {
        MediaEntry *e = &entries[i];
        const char *name = e->show_name ? e->show_name : e->title;
        int ok = 0;

        if (e->type == MEDIA_TYPE_MOVIE) {
            ok = fetch_movie_metadata(e->id, e->title);
        } else if (e->type == MEDIA_TYPE_EPISODE && e->show_name) {
            ok = fetch_episode_metadata(e->id, e->show_name, e->season, e->episode, e->year);
        }

        scrape_update(&tmdb_progress, name ? name : "?", ok > 0);
        database_free_entry(e);
    }

    free(entries);
    scrape_end(&tmdb_progress);
    printf("TMDB metadata fetch complete\n");
}

void scanner_rescan_all_tmdb(void) {
    if (!server_config.tmdb_api_key[0]) {
        printf("TMDB API key not configured, skipping rescan\n");
        return;
    }

    MediaEntry *entries = NULL;
    int count = 0;

    if (database_get_all_entries(&entries, &count) != 0 || count == 0) {
        printf("No entries to rescan\n");
        return;
    }

    printf("Re-fetching TMDB metadata for %d entries...\n", count);
    scrape_begin(&tmdb_progress, "tmdb_rescan", count);

    for (int i = 0; i < count; i++) {
        MediaEntry *e = &entries[i];
        const char *name = e->show_name ? e->show_name : e->title;
        int ok = 0;

        if (e->type == MEDIA_TYPE_MOVIE) {
            ok = fetch_movie_metadata(e->id, e->title);
        } else if ((e->type == MEDIA_TYPE_EPISODE || e->type == MEDIA_TYPE_TVSHOW) && e->show_name) {
            ok = fetch_episode_metadata(e->id, e->show_name, e->season, e->episode, e->year);
        }

        scrape_update(&tmdb_progress, name ? name : "?", ok > 0);
        database_free_entry(e);
    }

    free(entries);
    scrape_end(&tmdb_progress);
    printf("TMDB full rescan complete\n");
}

void scanner_refresh_show_status(void) {
    if (!server_config.tmdb_api_key[0]) return;

    int *ids = NULL;
    int count = 0;

    if (database_get_active_show_ids(&ids, &count) != 0 || count == 0) {
        free(ids);
        return;
    }

    printf("Refreshing status for %d active shows...\n", count);
    scrape_begin(&tmdb_progress, "show_status", count);

    for (int i = 0; i < count; i++) {
        TmdbTvShow *show = tmdb_get_show_status(ids[i]);
        if (show) {
            database_update_show_status(ids[i], show->status, show->next_episode_date,
                                        show->number_of_seasons, show->number_of_episodes,
                                        show->episode_run_time);
            scrape_update(&tmdb_progress, show->status ? show->status : "?", 1);
            printf("  Show %d: %s (%d seasons, %d episodes, %d min/ep, next: %s)\n",
                   ids[i],
                   show->status ? show->status : "?",
                   show->number_of_seasons, show->number_of_episodes,
                   show->episode_run_time,
                   show->next_episode_date ? show->next_episode_date : "none");
            tmdb_free_tvshow(show);
        } else {
            scrape_update(&tmdb_progress, "?", 0);
        }
    }

    free(ids);
    scrape_end(&tmdb_progress);
    printf("Show status refresh complete\n");
}

/* ==========================================================================
 * ROM Scanning
 * ========================================================================== */

/* ROM file extensions per console */
static const char *rom_extensions_nes[] = {".nes", ".fds", NULL};
static const char *rom_extensions_snes[] = {".sfc", ".smc", NULL};
static const char *rom_extensions_n64[] = {".z64", ".n64", ".v64", NULL};
static const char *rom_extensions_gc[] = {".iso", ".gcm", ".ciso", ".gcz", ".rvz", NULL};
static const char *rom_extensions_wii[] = {".iso", ".wbfs", ".ciso", ".gcz", ".rvz", NULL};
static const char *rom_extensions_gb[] = {".gb", NULL};
static const char *rom_extensions_gbc[] = {".gbc", NULL};
static const char *rom_extensions_gba[] = {".gba", NULL};
static const char *rom_extensions_ps2[] = {".iso", ".bin", ".chd", ".img", NULL};
static const char *rom_extensions_switch[] = {".nsp", ".xci", NULL};
static const char *rom_extensions_ps1[] = {".bin", ".iso", ".chd", ".img", ".pbp", NULL};
static const char *rom_extensions_ps3[] = {".iso", ".pkg", NULL};
static const char *rom_extensions_ps4[] = {".pkg", NULL};
static const char *rom_extensions_psp[] = {".iso", ".cso", ".chd", ".pbp", NULL};
static const char *rom_extensions_vita[] = {".vpk", NULL};
static const char *rom_extensions_xbox[] = {".iso", ".xiso", NULL};
static const char *rom_extensions_xbox360[] = {".iso", ".xex", NULL};
static const char *rom_extensions_wiiu[] = {".wud", ".wux", ".rpx", ".wua", NULL};
static const char *rom_extensions_ds[] = {".nds", ".srl", NULL};
static const char *rom_extensions_3ds[] = {".3ds", ".cia", ".cxi", NULL};
static const char *rom_extensions_genesis[] = {".md", ".gen", ".smd", ".bin", NULL};
static const char *rom_extensions_ms[] = {".sms", NULL};
static const char *rom_extensions_saturn[] = {".iso", ".chd", ".bin", NULL};
static const char *rom_extensions_dreamcast[] = {".gdi", ".cdi", ".chd", NULL};
static const char *rom_extensions_segacd[] = {".chd", ".iso", ".bin", NULL};
static const char *rom_extensions_atari2600[] = {".a26", ".bin", NULL};
static const char *rom_extensions_tgfx16[] = {".pce", ".sgx", ".chd", NULL};
static const char *rom_extensions_32x[] = {".32x", ".bin", NULL};
static const char *rom_extensions_gamegear[] = {".gg", NULL};

static const char **rom_ext_table[] = {
    rom_extensions_nes,        /* 0  NES */
    rom_extensions_snes,       /* 1  SNES */
    rom_extensions_n64,        /* 2  N64 */
    rom_extensions_gc,         /* 3  GameCube */
    rom_extensions_wii,        /* 4  Wii */
    rom_extensions_gb,         /* 5  Game Boy */
    rom_extensions_gbc,        /* 6  Game Boy Color */
    rom_extensions_gba,        /* 7  Game Boy Advance */
    rom_extensions_ps2,        /* 8  PS2 */
    rom_extensions_switch,     /* 9  Switch */
    rom_extensions_ps1,        /* 10 PS1 */
    rom_extensions_ps3,        /* 11 PS3 */
    rom_extensions_ps4,        /* 12 PS4 */
    rom_extensions_psp,        /* 13 PSP */
    rom_extensions_vita,       /* 14 Vita */
    rom_extensions_xbox,       /* 15 Xbox */
    rom_extensions_xbox360,    /* 16 Xbox 360 */
    rom_extensions_wiiu,       /* 17 Wii U */
    rom_extensions_ds,         /* 18 DS */
    rom_extensions_3ds,        /* 19 3DS */
    rom_extensions_genesis,    /* 20 Genesis */
    rom_extensions_ms,         /* 21 Master System */
    rom_extensions_saturn,     /* 22 Saturn */
    rom_extensions_dreamcast,  /* 23 Dreamcast */
    rom_extensions_segacd,     /* 24 Sega CD */
    rom_extensions_atari2600,  /* 25 Atari 2600 */
    rom_extensions_tgfx16,     /* 26 TurboGrafx-16 */
    rom_extensions_32x,        /* 27 32X */
    rom_extensions_gamegear,   /* 28 Game Gear */
};

/* ========================================================================
 * libretro-thumbnails cover art fallback
 * Downloads box art from the libretro-thumbnails GitHub repos.
 * Uses No-Intro naming convention to match ROM filenames.
 * ======================================================================== */

/* Map ConsoleType to libretro-thumbnails GitHub repo name */
static const char *libretro_console_repo(int console) {
    switch (console) {
    case CONSOLE_NES:           return "Nintendo_-_Nintendo_Entertainment_System";
    case CONSOLE_SNES:          return "Nintendo_-_Super_Nintendo_Entertainment_System";
    case CONSOLE_N64:           return "Nintendo_-_Nintendo_64";
    case CONSOLE_GAMECUBE:      return "Nintendo_-_GameCube";
    case CONSOLE_WII:           return "Nintendo_-_Wii";
    case CONSOLE_GB:            return "Nintendo_-_Game_Boy";
    case CONSOLE_GBC:           return "Nintendo_-_Game_Boy_Color";
    case CONSOLE_GBA:           return "Nintendo_-_Game_Boy_Advance";
    case CONSOLE_PS2:           return "Sony_-_PlayStation_2";
    case CONSOLE_SWITCH:        return "Nintendo_-_Switch";
    case CONSOLE_PS1:           return "Sony_-_PlayStation";
    case CONSOLE_PS3:           return "Sony_-_PlayStation_3";
    case CONSOLE_PS4:           return "Sony_-_PlayStation_4";
    case CONSOLE_PSP:           return "Sony_-_PlayStation_Portable";
    case CONSOLE_VITA:          return "Sony_-_PlayStation_Vita";
    case CONSOLE_XBOX:          return "Microsoft_-_Xbox";
    case CONSOLE_XBOX360:       return "Microsoft_-_Xbox_360";
    case CONSOLE_WIIU:          return "Nintendo_-_Wii_U";
    case CONSOLE_DS:            return "Nintendo_-_Nintendo_DS";
    case CONSOLE_3DS:           return "Nintendo_-_Nintendo_3DS";
    case CONSOLE_GENESIS:       return "Sega_-_Mega_Drive_-_Genesis";
    case CONSOLE_MASTER_SYSTEM: return "Sega_-_Master_System_-_Mark_III";
    case CONSOLE_SATURN:        return "Sega_-_Saturn";
    case CONSOLE_DREAMCAST:     return "Sega_-_Dreamcast";
    case CONSOLE_SEGACD:        return "Sega_-_Mega-CD_-_Sega_CD";
    case CONSOLE_ATARI2600:     return "Atari_-_2600";
    case CONSOLE_TGFX16:       return "NEC_-_PC_Engine_-_TurboGrafx_16";
    case CONSOLE_32X:           return "Sega_-_32X";
    case CONSOLE_GAMEGEAR:      return "Sega_-_Game_Gear";
    default: return NULL;
    }
}

/* URL-encode a filename for use in a URL.
 * Encodes spaces, parentheses, ampersands, and other special characters. */
static void url_encode(const char *src, char *dst, size_t dst_size) {
    static const char *safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~";
    char *d = dst;
    char *end = dst + dst_size - 4;
    while (*src && d < end) {
        if (strchr(safe, *src)) {
            *d++ = *src;
        } else {
            snprintf(d, 4, "%%%02X", (unsigned char)*src);
            d += 3;
        }
        src++;
    }
    *d = '\0';
}

/* Try downloading a cover from libretro-thumbnails GitHub.
 * filepath: full path to the ROM file
 * console: ConsoleType enum
 * igdb_id: IGDB game ID (0 if unknown)
 * rom_id: ROM database ID (fallback for filename)
 * cache_dir: directory to save the cover image
 * Returns allocated path to the cover file, or NULL on failure. */
static char *try_libretro_cover(const char *filepath, int console, int igdb_id,
                                 int rom_id, const char *cache_dir) {
    const char *repo = libretro_console_repo(console);
    if (!repo) return NULL;

    /* Extract ROM filename without extension */
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    char rom_name[512];
    strncpy(rom_name, base, sizeof(rom_name) - 1);
    rom_name[sizeof(rom_name) - 1] = '\0';

    /* Strip archive extension — for .7z/.zip/.rar, use inner filename concept
     * but since we don't know the inner name, use the archive name itself */
    char *ext = strrchr(rom_name, '.');
    if (ext) *ext = '\0';

    if (rom_name[0] == '\0') return NULL;

    /* Use igdb_id for filename if available, else ROM db id */
    char local_path[4096];
    if (igdb_id > 0)
        snprintf(local_path, sizeof(local_path), "%s/cover_%d.png",
                 cache_dir, igdb_id);
    else
        snprintf(local_path, sizeof(local_path), "%s/cover_r%d.png",
                 cache_dir, rom_id);

    struct stat st;
    if (stat(local_path, &st) == 0 && st.st_size > 100)
        return strdup(local_path);

    /* Also check .jpg variant (IGDB may have saved one) */
    char jpg_path[4096];
    if (igdb_id > 0)
        snprintf(jpg_path, sizeof(jpg_path), "%s/cover_%d.jpg",
                 cache_dir, igdb_id);
    else
        snprintf(jpg_path, sizeof(jpg_path), "%s/cover_r%d.jpg",
                 cache_dir, rom_id);
    if (stat(jpg_path, &st) == 0 && st.st_size > 100)
        return strdup(jpg_path);

    /* URL-encode the ROM name for the GitHub URL */
    char encoded_name[2048];
    url_encode(rom_name, encoded_name, sizeof(encoded_name));

    /* Construct GitHub raw URL:
     * https://raw.githubusercontent.com/libretro-thumbnails/{repo}/master/Named_Boxarts/{name}.png */
    char url[4096];
    snprintf(url, sizeof(url),
        "https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s.png",
        repo, encoded_name);

    mkdir(cache_dir, 0755);

    CURL *dl = curl_easy_init();
    if (!dl) return NULL;

    FILE *f = fopen(local_path, "wb");
    if (!f) {
        curl_easy_cleanup(dl);
        return NULL;
    }

    curl_easy_setopt(dl, CURLOPT_URL, url);
    curl_easy_setopt(dl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(dl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(dl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(dl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(dl);
    curl_easy_cleanup(dl);
    fclose(f);

    if (res == CURLE_OK && stat(local_path, &st) == 0 && st.st_size > 100)
        return strdup(local_path);

    unlink(local_path);
    return NULL;
}

/* All ROM extensions combined for quick check */
int scanner_is_rom_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;

    if (strcasecmp(ext, ".zip") == 0 ||
        strcasecmp(ext, ".7z") == 0 ||
        strcasecmp(ext, ".rar") == 0)
        return 1;

    for (int c = 0; c < CONSOLE_COUNT; c++) {
        for (int i = 0; rom_ext_table[c][i]; i++) {
            if (strcasecmp(ext, rom_ext_table[c][i]) == 0)
                return 1;
        }
    }
    return 0;
}

/* Detect GB vs GBC by reading the CGB flag at ROM header offset 0x143.
 * 0x80 = GBC-compatible (dual mode, works on both GB and GBC)
 * 0xC0 = GBC-only
 * Anything else = original Game Boy */
static int scanner_detect_gb_vs_gbc(const unsigned char *header, size_t len) {
    if (len < 0x144) return CONSOLE_GB;
    unsigned char cgb = header[0x143];
    if (cgb == 0x80 || cgb == 0xC0)
        return CONSOLE_GBC;
    return CONSOLE_GB;
}

/* Read CGB flag from a loose .gb file */
static int scanner_detect_gb_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return CONSOLE_GB;
    unsigned char header[0x144];
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    return scanner_detect_gb_vs_gbc(header, n);
}

/* Detect if a disc image is GameCube or Wii by reading magic bytes.
 * GameCube: magic 0xC2339F3D at offset 0x1C
 * Wii:      magic 0x5D1C9EA3 at offset 0x18
 * Returns CONSOLE_GAMECUBE, CONSOLE_WII, or -1 if unknown */
static int scanner_detect_disc_type(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char header[0x20];
    if (fread(header, 1, sizeof(header), f) < sizeof(header)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Check Wii magic at offset 0x18 */
    uint32_t wii_magic = ((uint32_t)header[0x18] << 24) | ((uint32_t)header[0x19] << 16) |
                         ((uint32_t)header[0x1A] << 8)  | (uint32_t)header[0x1B];
    if (wii_magic == 0x5D1C9EA3)
        return CONSOLE_WII;

    /* Check GameCube magic at offset 0x1C */
    uint32_t gc_magic = ((uint32_t)header[0x1C] << 24) | ((uint32_t)header[0x1D] << 16) |
                        ((uint32_t)header[0x1E] << 8)  | (uint32_t)header[0x1F];
    if (gc_magic == 0xC2339F3D)
        return CONSOLE_GAMECUBE;

    return -1;
}

/* Detect console type by inspecting filenames inside a .zip archive.
 * Excludes GC/Wii since dolphin-emu doesn't support .zip input.
 * For .gb files, reads the ROM header to distinguish GB vs GBC. */
static int scanner_detect_console_from_zip(const char *path) {
    int err;
    zip_t *za = zip_open(path, ZIP_RDONLY, &err);
    if (!za) return -1;

    int result = -1;
    zip_int64_t match_idx = -1;
    zip_int64_t n = zip_get_num_entries(za, 0);

    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(za, i, 0);
        if (!name) continue;

        const char *ext = strrchr(name, '.');
        if (!ext) continue;

        for (int c = 0; c < CONSOLE_COUNT; c++) {
            /* Skip disc-based consoles — emulators don't support disc images in .zip */
            if (c == CONSOLE_GAMECUBE || c == CONSOLE_WII || c == CONSOLE_WIIU ||
                c == CONSOLE_PS1 || c == CONSOLE_PS2 || c == CONSOLE_PS3 || c == CONSOLE_PS4 ||
                c == CONSOLE_PSP || c == CONSOLE_VITA ||
                c == CONSOLE_XBOX || c == CONSOLE_XBOX360 ||
                c == CONSOLE_SATURN || c == CONSOLE_DREAMCAST || c == CONSOLE_SEGACD)
                continue;
            for (int j = 0; rom_ext_table[c][j]; j++) {
                if (strcasecmp(ext, rom_ext_table[c][j]) == 0) {
                    result = c;
                    match_idx = i;
                    goto done;
                }
            }
        }
    }

done:
    /* For .gb files inside zip, read ROM header to distinguish GB vs GBC */
    if (result == CONSOLE_GB && match_idx >= 0) {
        zip_file_t *zf = zip_fopen_index(za, match_idx, 0);
        if (zf) {
            unsigned char header[0x144];
            zip_int64_t rd = zip_fread(zf, header, sizeof(header));
            if (rd >= (zip_int64_t)sizeof(header))
                result = scanner_detect_gb_vs_gbc(header, rd);
            zip_fclose(zf);
        }
    }

    zip_close(za);
    return result;
}

/* Detect console type by inspecting filenames inside a .7z/.rar archive via libarchive.
 * Same logic as scanner_detect_console_from_zip but using libarchive API.
 * For .gb files, reads the ROM header to distinguish GB vs GBC. */
static int scanner_detect_console_from_archive(const char *path) {
    struct archive *a = archive_read_new();
    if (!a) return -1;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, path, 16384) != ARCHIVE_OK) {
        archive_read_free(a);
        return -1;
    }

    int result = -1;
    int need_gb_header = 0;

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!name) { archive_read_data_skip(a); continue; }

        const char *ext = strrchr(name, '.');
        if (!ext) { archive_read_data_skip(a); continue; }

        for (int c = 0; c < CONSOLE_COUNT; c++) {
            /* Skip disc-based consoles — emulators don't support disc images in archives */
            if (c == CONSOLE_GAMECUBE || c == CONSOLE_WII || c == CONSOLE_WIIU ||
                c == CONSOLE_PS1 || c == CONSOLE_PS2 || c == CONSOLE_PS3 || c == CONSOLE_PS4 ||
                c == CONSOLE_PSP || c == CONSOLE_VITA ||
                c == CONSOLE_XBOX || c == CONSOLE_XBOX360 ||
                c == CONSOLE_SATURN || c == CONSOLE_DREAMCAST || c == CONSOLE_SEGACD)
                continue;
            for (int j = 0; rom_ext_table[c][j]; j++) {
                if (strcasecmp(ext, rom_ext_table[c][j]) == 0) {
                    result = c;
                    if (c == CONSOLE_GB) need_gb_header = 1;
                    goto archive_done;
                }
            }
        }
        archive_read_data_skip(a);
    }

archive_done:
    /* For .gb files inside archive, read ROM header to distinguish GB vs GBC */
    if (need_gb_header && result == CONSOLE_GB) {
        unsigned char header[0x144];
        ssize_t rd = archive_read_data(a, header, sizeof(header));
        if (rd >= (ssize_t)sizeof(header))
            result = scanner_detect_gb_vs_gbc(header, rd);
    }

    archive_read_free(a);
    return result;
}

/* Try to infer console type from parent directory name.
 * Matches common directory names like "GBA", "Game Boy Advance", "SNES", etc. */
static int scanner_detect_console_from_dir(const char *path) {
    /* Find parent directory name */
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;

    /* Walk back to find the start of the parent dir */
    const char *end = slash;
    const char *start = end - 1;
    while (start > path && *start != '/') start--;
    if (*start == '/') start++;

    size_t len = end - start;
    char dir[64];
    if (len == 0 || len >= sizeof(dir)) return -1;
    memcpy(dir, start, len);
    dir[len] = '\0';

    if (strcasecmp(dir, "NES") == 0) return CONSOLE_NES;
    if (strcasecmp(dir, "SNES") == 0 || strcasecmp(dir, "Super Nintendo") == 0) return CONSOLE_SNES;
    if (strcasecmp(dir, "N64") == 0 || strcasecmp(dir, "Nintendo 64") == 0) return CONSOLE_N64;
    if (strcasecmp(dir, "GameCube") == 0 || strcasecmp(dir, "GC") == 0) return CONSOLE_GAMECUBE;
    if (strcasecmp(dir, "Wii") == 0) return CONSOLE_WII;
    if (strcasecmp(dir, "GBA") == 0 || strcasecmp(dir, "Game Boy Advance") == 0) return CONSOLE_GBA;
    if (strcasecmp(dir, "GBC") == 0 || strcasecmp(dir, "Game Boy Color") == 0) return CONSOLE_GBC;
    if (strcasecmp(dir, "GB") == 0 || strcasecmp(dir, "Game Boy") == 0) return CONSOLE_GB;
    if (strcasecmp(dir, "PS2") == 0 || strcasecmp(dir, "PlayStation 2") == 0 ||
        strcasecmp(dir, "PlayStation2") == 0) return CONSOLE_PS2;
    if (strcasecmp(dir, "Switch") == 0 || strcasecmp(dir, "Nintendo Switch") == 0 ||
        strcasecmp(dir, "NSP") == 0) return CONSOLE_SWITCH;
    if (strcasecmp(dir, "PS1") == 0 || strcasecmp(dir, "PSX") == 0 ||
        strcasecmp(dir, "PlayStation") == 0 || strcasecmp(dir, "PlayStation 1") == 0 ||
        strcasecmp(dir, "PlayStation1") == 0) return CONSOLE_PS1;
    if (strcasecmp(dir, "PS3") == 0 || strcasecmp(dir, "PlayStation 3") == 0 ||
        strcasecmp(dir, "PlayStation3") == 0) return CONSOLE_PS3;
    if (strcasecmp(dir, "PS4") == 0 || strcasecmp(dir, "PlayStation 4") == 0 ||
        strcasecmp(dir, "PlayStation4") == 0) return CONSOLE_PS4;
    if (strcasecmp(dir, "PSP") == 0 || strcasecmp(dir, "PlayStation Portable") == 0) return CONSOLE_PSP;
    if (strcasecmp(dir, "Vita") == 0 || strcasecmp(dir, "PSVita") == 0 ||
        strcasecmp(dir, "PS Vita") == 0) return CONSOLE_VITA;
    if (strcasecmp(dir, "Xbox") == 0) return CONSOLE_XBOX;
    if (strcasecmp(dir, "Xbox 360") == 0 || strcasecmp(dir, "Xbox360") == 0 ||
        strcasecmp(dir, "X360") == 0) return CONSOLE_XBOX360;
    if (strcasecmp(dir, "Wii U") == 0 || strcasecmp(dir, "WiiU") == 0) return CONSOLE_WIIU;
    if (strcasecmp(dir, "DS") == 0 || strcasecmp(dir, "NDS") == 0 ||
        strcasecmp(dir, "Nintendo DS") == 0) return CONSOLE_DS;
    if (strcasecmp(dir, "3DS") == 0 || strcasecmp(dir, "Nintendo 3DS") == 0) return CONSOLE_3DS;
    if (strcasecmp(dir, "Genesis") == 0 || strcasecmp(dir, "Mega Drive") == 0 ||
        strcasecmp(dir, "MegaDrive") == 0 || strcasecmp(dir, "MD") == 0 ||
        strcasecmp(dir, "Sega Genesis") == 0) return CONSOLE_GENESIS;
    if (strcasecmp(dir, "Master System") == 0 || strcasecmp(dir, "SMS") == 0 ||
        strcasecmp(dir, "Sega Master System") == 0) return CONSOLE_MASTER_SYSTEM;
    if (strcasecmp(dir, "Saturn") == 0 || strcasecmp(dir, "Sega Saturn") == 0) return CONSOLE_SATURN;
    if (strcasecmp(dir, "Dreamcast") == 0 || strcasecmp(dir, "DC") == 0) return CONSOLE_DREAMCAST;
    if (strcasecmp(dir, "Sega CD") == 0 || strcasecmp(dir, "SegaCD") == 0 ||
        strcasecmp(dir, "Mega CD") == 0 || strcasecmp(dir, "MegaCD") == 0) return CONSOLE_SEGACD;
    if (strcasecmp(dir, "Atari 2600") == 0 || strcasecmp(dir, "Atari2600") == 0 ||
        strcasecmp(dir, "2600") == 0) return CONSOLE_ATARI2600;
    if (strcasecmp(dir, "TurboGrafx-16") == 0 || strcasecmp(dir, "TurboGrafx16") == 0 ||
        strcasecmp(dir, "TG16") == 0 || strcasecmp(dir, "PC Engine") == 0 ||
        strcasecmp(dir, "PCEngine") == 0) return CONSOLE_TGFX16;
    if (strcasecmp(dir, "32X") == 0 || strcasecmp(dir, "Sega 32X") == 0) return CONSOLE_32X;
    if (strcasecmp(dir, "Game Gear") == 0 || strcasecmp(dir, "GameGear") == 0 ||
        strcasecmp(dir, "GG") == 0) return CONSOLE_GAMEGEAR;

    return -1;
}

/* Detect console type from file extension and metadata */
int scanner_detect_console(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return -1;

    /* .zip — inspect contents, fall back to directory name */
    if (strcasecmp(ext, ".zip") == 0) {
        int result = scanner_detect_console_from_zip(path);
        if (result < 0)
            result = scanner_detect_console_from_dir(path);
        return result;
    }

    /* .7z / .rar — inspect contents via libarchive, fall back to directory name */
    if (strcasecmp(ext, ".7z") == 0 || strcasecmp(ext, ".rar") == 0) {
        int result = scanner_detect_console_from_archive(path);
        if (result < 0)
            result = scanner_detect_console_from_dir(path);
        return result;
    }

    /* Unique extensions — unambiguous console detection */
    if (strcasecmp(ext, ".nes") == 0 || strcasecmp(ext, ".fds") == 0) return CONSOLE_NES;
    if (strcasecmp(ext, ".sfc") == 0 || strcasecmp(ext, ".smc") == 0) return CONSOLE_SNES;
    if (strcasecmp(ext, ".z64") == 0 || strcasecmp(ext, ".n64") == 0 || strcasecmp(ext, ".v64") == 0) return CONSOLE_N64;
    if (strcasecmp(ext, ".gbc") == 0) return CONSOLE_GBC;
    if (strcasecmp(ext, ".gba") == 0) return CONSOLE_GBA;
    if (strcasecmp(ext, ".wbfs") == 0) return CONSOLE_WII;
    if (strcasecmp(ext, ".gcm") == 0) return CONSOLE_GAMECUBE;
    if (strcasecmp(ext, ".nsp") == 0 || strcasecmp(ext, ".xci") == 0) return CONSOLE_SWITCH;
    if (strcasecmp(ext, ".nds") == 0 || strcasecmp(ext, ".srl") == 0) return CONSOLE_DS;
    if (strcasecmp(ext, ".3ds") == 0 || strcasecmp(ext, ".cia") == 0 ||
        strcasecmp(ext, ".cxi") == 0) return CONSOLE_3DS;
    if (strcasecmp(ext, ".cso") == 0 || strcasecmp(ext, ".pbp") == 0) return CONSOLE_PSP;
    if (strcasecmp(ext, ".vpk") == 0) return CONSOLE_VITA;
    if (strcasecmp(ext, ".xiso") == 0) return CONSOLE_XBOX;
    if (strcasecmp(ext, ".xex") == 0) return CONSOLE_XBOX360;
    if (strcasecmp(ext, ".wud") == 0 || strcasecmp(ext, ".wux") == 0 ||
        strcasecmp(ext, ".rpx") == 0 || strcasecmp(ext, ".wua") == 0) return CONSOLE_WIIU;
    if (strcasecmp(ext, ".md") == 0 || strcasecmp(ext, ".gen") == 0 ||
        strcasecmp(ext, ".smd") == 0) return CONSOLE_GENESIS;
    if (strcasecmp(ext, ".sms") == 0) return CONSOLE_MASTER_SYSTEM;
    if (strcasecmp(ext, ".gg") == 0) return CONSOLE_GAMEGEAR;
    if (strcasecmp(ext, ".32x") == 0) return CONSOLE_32X;
    if (strcasecmp(ext, ".gdi") == 0 || strcasecmp(ext, ".cdi") == 0) return CONSOLE_DREAMCAST;
    if (strcasecmp(ext, ".a26") == 0) return CONSOLE_ATARI2600;
    if (strcasecmp(ext, ".pce") == 0 || strcasecmp(ext, ".sgx") == 0) return CONSOLE_TGFX16;

    /* .gb — read ROM header to distinguish GB vs GBC */
    if (strcasecmp(ext, ".gb") == 0)
        return scanner_detect_gb_file(path);

    /* Shared extensions used by multiple disc-based consoles —
     * try GC/Wii magic bytes first, fall back to parent directory name.
     * Compressed formats (.rvz, .gcz, .chd) won't have raw disc headers
     * at standard offsets, so directory fallback is essential. */
    if (strcasecmp(ext, ".iso") == 0 || strcasecmp(ext, ".ciso") == 0 ||
        strcasecmp(ext, ".gcz") == 0 || strcasecmp(ext, ".rvz") == 0 ||
        strcasecmp(ext, ".chd") == 0 || strcasecmp(ext, ".bin") == 0 ||
        strcasecmp(ext, ".img") == 0 || strcasecmp(ext, ".pkg") == 0) {
        int result = scanner_detect_disc_type(path);
        if (result >= 0) return result;
        return scanner_detect_console_from_dir(path);
    }

    return -1;
}

/* Extract clean ROM title from filename */
static void scanner_extract_rom_title(const char *filename, char *title, size_t title_size) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    strncpy(title, base, title_size - 1);
    title[title_size - 1] = '\0';

    /* Remove extension */
    char *ext = strrchr(title, '.');
    if (ext) *ext = '\0';

    /* Remove region/revision tags in parentheses/brackets: (USA), [!], (Rev 1), etc. */
    char *p = title;
    char *dst = title;
    int in_bracket = 0;
    while (*p) {
        if (*p == '(' || *p == '[') {
            /* Trim trailing spaces before bracket */
            while (dst > title && *(dst - 1) == ' ') dst--;
            in_bracket++;
        } else if ((*p == ')' || *p == ']') && in_bracket > 0) {
            in_bracket--;
            /* Skip space after closing bracket */
            if (*(p + 1) == ' ') p++;
        } else if (!in_bracket) {
            *dst++ = *p;
        }
        p++;
    }
    *dst = '\0';

    /* Trim trailing spaces */
    size_t len = strlen(title);
    while (len > 0 && title[len - 1] == ' ') title[--len] = '\0';
}

/* Extract region from filename parentheses */
static void scanner_extract_region(const char *filename, char *region, size_t region_size) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    region[0] = '\0';

    /* Look for (USA), (Europe), (Japan), etc. */
    const char *p = base;
    while ((p = strchr(p, '(')) != NULL) {
        p++;
        const char *end = strchr(p, ')');
        if (!end) break;

        size_t len = end - p;
        if (len < region_size) {
            /* Check if it looks like a region */
            if (strncasecmp(p, "USA", 3) == 0 ||
                strncasecmp(p, "Europe", 6) == 0 ||
                strncasecmp(p, "Japan", 5) == 0 ||
                strncasecmp(p, "World", 5) == 0 ||
                strncasecmp(p, "En", 2) == 0) {
                strncpy(region, p, len);
                region[len] = '\0';
                return;
            }
        }
        p = end + 1;
    }
}

int scanner_scan_rom_file(const char *filepath, int console) {
    if (database_rom_exists(filepath))
        return 0;

    char title[256];
    char region[64];
    scanner_extract_rom_title(filepath, title, sizeof(title));
    scanner_extract_region(filepath, region, sizeof(region));

    struct stat st;
    int64_t size = 0;
    if (stat(filepath, &st) == 0)
        size = st.st_size;

    RomEntry entry = {0};
    entry.console = console;
    entry.title = title;
    entry.filepath = (char *)filepath;
    entry.cover_path = NULL;
    entry.size = size;
    entry.region = region[0] ? region : NULL;

    int rc = database_add_rom(&entry);
    if (rc == 0) {
        printf("  ROM: %s [%s] %s\n", title,
               database_console_name(console),
               region[0] ? region : "");
    }
    return rc;
}

/* Recursively scan directory for ROM files */
int scanner_scan_rom_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        /* Not an error if directory doesn't exist yet */
        return 0;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            count += scanner_scan_rom_directory(fullpath);
        } else if (S_ISREG(st.st_mode) && scanner_is_rom_file(fullpath)) {
            int console = scanner_detect_console(fullpath);
            if (console >= 0) {
                if (scanner_scan_rom_file(fullpath, console) == 0)
                    count++;
            }
        }
    }

    closedir(dir);
    return count;
}

/* Forward declaration */
static char *download_igdb_cover(const char *url, int igdb_id,
                                  const char *cache_dir);

/* Fetch cover images for ROMs that have IGDB metadata but no cover.
 * This retries cover downloads for ROMs where the initial search response
 * didn't include cover data, using a targeted IGDB query by game ID. */
void scanner_fetch_rom_covers(void) {
    if (!igdb_is_available()) return;

    int *ids = NULL, *consoles = NULL, *igdb_ids = NULL;
    char **titles = NULL;
    int count = 0;

    if (database_get_roms_without_cover(&ids, &titles, &consoles, &igdb_ids, &count) != 0
        || count == 0) {
        free(ids); free(titles); free(consoles); free(igdb_ids);
        return;
    }

    printf("IGDB: Fetching covers for %d ROMs...\n", count);
    scrape_begin(&igdb_progress, "igdb_covers", count);

    int fetched = 0;
    for (int i = 0; i < count; i++) {
        char *cover_url = igdb_get_game_cover(igdb_ids[i]);
        if (cover_url) {
            char *local = download_igdb_cover(cover_url, igdb_ids[i],
                                               server_config.cache_dir);
            if (local) {
                database_update_rom_cover(ids[i], local);
                printf("  IGDB cover: %s [OK]\n", titles[i]);
                free(local);
                fetched++;
                scrape_update(&igdb_progress, titles[i], 1);
            } else {
                printf("  IGDB cover: %s [download failed]\n", titles[i]);
                scrape_update(&igdb_progress, titles[i], 0);
            }
            free(cover_url);
        } else {
            printf("  IGDB cover: %s [no cover on IGDB]\n", titles[i]);
            scrape_update(&igdb_progress, titles[i], 0);
        }
        usleep(260000);
    }

    for (int i = 0; i < count; i++) free(titles[i]);
    free(ids); free(titles); free(consoles); free(igdb_ids);

    scrape_end(&igdb_progress);
    printf("IGDB: Cover fetch complete (%d/%d downloaded)\n", fetched, count);
}

/* Fetch cover art from libretro-thumbnails GitHub for ALL ROMs without covers.
 * This is a fallback that works even when IGDB didn't match the ROM.
 * Uses the original ROM filename (No-Intro naming convention) to download. */
void scanner_fetch_libretro_covers(void) {
    int *ids = NULL, *consoles = NULL, *igdb_ids = NULL;
    char **filepaths = NULL;
    int count = 0;

    if (database_get_all_roms_without_cover(&ids, &filepaths, &consoles, &igdb_ids, &count) != 0
        || count == 0) {
        free(ids); free(filepaths); free(consoles); free(igdb_ids);
        return;
    }

    printf("libretro-thumbnails: Fetching covers for %d ROMs...\n", count);
    scrape_begin(&igdb_progress, "libretro_covers", count);

    int fetched = 0;
    for (int i = 0; i < count; i++) {
        char *cover = try_libretro_cover(filepaths[i], consoles[i],
                                          igdb_ids[i], ids[i],
                                          server_config.cache_dir);
        if (cover) {
            database_update_rom_cover(ids[i], cover);
            /* Extract basename for logging */
            const char *base = strrchr(filepaths[i], '/');
            base = base ? base + 1 : filepaths[i];
            printf("  libretro cover: %s [OK]\n", base);
            free(cover);
            fetched++;
            scrape_update(&igdb_progress, base, 1);
        } else {
            scrape_update(&igdb_progress, "", 0);
        }

        /* Small delay to avoid hammering GitHub */
        if (fetched > 0 && fetched % 50 == 0)
            usleep(500000);
    }

    for (int i = 0; i < count; i++) free(filepaths[i]);
    free(ids); free(filepaths); free(consoles); free(igdb_ids);

    scrape_end(&igdb_progress);
    printf("libretro-thumbnails: Cover fetch complete (%d/%d downloaded)\n", fetched, count);
}

/* Clean a ROM title for IGDB search: strip region tags, version tags, etc. */
static void clean_rom_title(const char *raw, char *out, size_t out_size) {
    strncpy(out, raw, out_size - 1);
    out[out_size - 1] = '\0';

    /* Strip leading collection index number: "1865 - Game Name" → "Game Name"
     * Pattern: 3+ digits followed by " - " at start of title.
     * Skip if the number looks like a game year (1900-2100) AND
     * what follows is a subtitle (these are real games like "1943 - The Battle of Midway") */
    {
        const char *p = out;
        int ndigits = 0;
        while (*p && isdigit((unsigned char)*p)) { p++; ndigits++; }
        if (ndigits >= 3 && strncmp(p, " - ", 3) == 0) {
            int num = atoi(out);
            /* If it's NOT a plausible game year (1900-2100), always strip.
             * If it IS a plausible year, don't strip — it might be a real game title.
             * Exception: leading zeros (0006, 0150) are ALWAYS collection indices
             * (real years never have leading zeros), and 5+ digit numbers too. */
            int has_leading_zero = (ndigits >= 4 && out[0] == '0');
            if (num < 1900 || num > 2100 || has_leading_zero || ndigits > 4) {
                memmove(out, p + 3, strlen(p + 3) + 1);
            }
        }
    }

    /* Strip anything in parentheses: (USA), (Europe), (Rev A), (V1.1), etc. */
    char *p;
    while ((p = strchr(out, '(')) != NULL) {
        char *close = strchr(p, ')');
        if (close) {
            /* Remove from '(' to ')' inclusive, plus trailing space */
            char *after = close + 1;
            while (*after == ' ') after++;
            memmove(p, after, strlen(after) + 1);
        } else {
            *p = '\0'; /* Unclosed paren, just truncate */
            break;
        }
    }

    /* Strip anything in brackets: [!], [b1], etc. */
    while ((p = strchr(out, '[')) != NULL) {
        char *close = strchr(p, ']');
        if (close) {
            char *after = close + 1;
            while (*after == ' ') after++;
            memmove(p, after, strlen(after) + 1);
        } else {
            *p = '\0';
            break;
        }
    }

    /* Trim trailing whitespace */
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) {
        out[--len] = '\0';
    }

    /* Strip "Disc N" / "Disk N" / "CD N" suffix (multi-disc games) */
    {
        /* Match patterns like "Disc 1", "Disc1", "Disk 2", "CD2" at end */
        char *disc = NULL;
        for (int try = 0; try < 3 && !disc; try++) {
            const char *patterns[] = {"Disc ", "Disk ", "CD "};
            const char *patterns_no_space[] = {"Disc", "Disk", "CD"};
            size_t plen[] = {5, 5, 3};
            size_t plen_ns[] = {4, 4, 2};
            len = strlen(out);
            /* Try "Disc N" with space */
            if (len > plen[try] + 1) {
                char *candidate = out + len - plen[try] - 1;
                if (strncasecmp(candidate, patterns[try], plen[try]) == 0 &&
                    isdigit((unsigned char)candidate[plen[try]])) {
                    disc = candidate;
                }
            }
            /* Try "DiscN" without space */
            if (!disc && len > plen_ns[try] + 1) {
                char *candidate = out + len - plen_ns[try] - 1;
                if (strncasecmp(candidate, patterns_no_space[try], plen_ns[try]) == 0 &&
                    isdigit((unsigned char)candidate[plen_ns[try]])) {
                    disc = candidate;
                }
            }
        }
        if (disc) {
            /* Trim back trailing spaces/dashes before "Disc" */
            while (disc > out && (*(disc - 1) == ' ' || *(disc - 1) == '-')) disc--;
            *disc = '\0';
        }
    }

    /* Trim trailing whitespace again after disc strip */
    len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) {
        out[--len] = '\0';
    }

    /* Strip "Disney's " or "Disney " prefix (IGDB rarely has these) */
    if (strncasecmp(out, "Disney's ", 9) == 0) {
        memmove(out, out + 9, strlen(out + 9) + 1);
    } else if (strncasecmp(out, "Disney ", 7) == 0) {
        memmove(out, out + 7, strlen(out + 7) + 1);
    }

    /* Handle trailing ", The" → prepend "The " (some No-Intro dumps) */
    len = strlen(out);
    if (len > 5 && strcasecmp(out + len - 5, ", The") == 0) {
        char temp[256];
        out[len - 5] = '\0'; /* Remove ", The" */
        snprintf(temp, sizeof(temp), "The %s", out);
        strncpy(out, temp, out_size - 1);
        out[out_size - 1] = '\0';
    }

    /* Handle trailing ", A" → prepend "A " */
    len = strlen(out);
    if (len > 3 && strcasecmp(out + len - 3, ", A") == 0) {
        char temp[256];
        out[len - 3] = '\0';
        snprintf(temp, sizeof(temp), "A %s", out);
        strncpy(out, temp, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* Generate search variations for a ROM title.
 * No-Intro naming uses " - " as subtitle separator, but actual game titles
 * often use ": " (e.g., "1943 - The Battle of Midway" -> "1943: The Battle of Midway").
 * Also tries without subtitle for short-named games (e.g., just "1943"). */
static void generate_rom_variations(const char *clean, char variations[][256], int *count) {
    *count = 0;

    /* Variation 0: original cleaned title */
    strncpy(variations[(*count)++], clean, 255);
    variations[*count - 1][255] = '\0';

    /* Variation 1: replace " - " with ": " (No-Intro -> real title) */
    char *dash = strstr(variations[0], " - ");
    if (dash && *count < 14) {
        char temp[256];
        size_t prefix_len = dash - variations[0];
        memcpy(temp, variations[0], prefix_len);
        temp[prefix_len] = ':';
        temp[prefix_len + 1] = ' ';
        strncpy(temp + prefix_len + 2, dash + 3, sizeof(temp) - prefix_len - 3);
        temp[sizeof(temp) - 1] = '\0';
        strncpy(variations[(*count)++], temp, 255);
        variations[*count - 1][255] = '\0';
    }

    /* Variation 2: replace " - " with just " " */
    if (dash && *count < 14) {
        char temp[256];
        size_t prefix_len = dash - variations[0];
        memcpy(temp, variations[0], prefix_len);
        temp[prefix_len] = ' ';
        strncpy(temp + prefix_len + 1, dash + 3, sizeof(temp) - prefix_len - 2);
        temp[sizeof(temp) - 1] = '\0';
        strncpy(variations[(*count)++], temp, 255);
        variations[*count - 1][255] = '\0';
    }

    /* Variation 3: just the part before " - " (subtitle stripped) */
    if (dash && *count < 14) {
        char temp[256];
        size_t prefix_len = dash - variations[0];
        if (prefix_len > 0 && prefix_len < sizeof(temp)) {
            memcpy(temp, variations[0], prefix_len);
            temp[prefix_len] = '\0';
            strncpy(variations[(*count)++], temp, 255);
            variations[*count - 1][255] = '\0';
        }
    }

    /* Variation 4: swap & ↔ and */
    if (*count < 14) {
        char temp[256];
        strncpy(temp, clean, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
        int changed = 0;

        /* Try replacing " & " with " and " */
        char *amp = strstr(temp, " & ");
        if (amp) {
            char buf[256];
            size_t pre = amp - temp;
            memcpy(buf, temp, pre);
            memcpy(buf + pre, " and ", 5);
            strncpy(buf + pre + 5, amp + 3, sizeof(buf) - pre - 6);
            buf[sizeof(buf) - 1] = '\0';
            strncpy(variations[(*count)++], buf, 255);
            variations[*count - 1][255] = '\0';
            changed = 1;
        }

        /* Try replacing " and " with " & " */
        if (!changed) {
            char *lower = temp;
            /* Case-insensitive search for " and " */
            while (*lower) {
                if ((lower[0] == ' ') &&
                    (lower[1] == 'a' || lower[1] == 'A') &&
                    (lower[2] == 'n' || lower[2] == 'N') &&
                    (lower[3] == 'd' || lower[3] == 'D') &&
                    (lower[4] == ' ')) {
                    char buf[256];
                    size_t pre = lower - temp;
                    memcpy(buf, temp, pre);
                    memcpy(buf + pre, " & ", 3);
                    strncpy(buf + pre + 3, lower + 5, sizeof(buf) - pre - 4);
                    buf[sizeof(buf) - 1] = '\0';
                    strncpy(variations[(*count)++], buf, 255);
                    variations[*count - 1][255] = '\0';
                    break;
                }
                lower++;
            }
        }
    }

    /* Variation 5: strip leading "The " */
    if (*count < 14 && strncasecmp(clean, "The ", 4) == 0 && clean[4] != '\0') {
        strncpy(variations[(*count)++], clean + 4, 255);
        variations[*count - 1][255] = '\0';
    }

    /* Variation 6: strip leading year-like number prefix if present
     * "1943 - The Battle of Midway" → "The Battle of Midway"
     * (clean_rom_title only strips non-year numbers; this catches year-range too) */
    if (*count < 14) {
        const char *p = clean;
        int ndigits = 0;
        while (*p && isdigit((unsigned char)*p)) { p++; ndigits++; }
        if (ndigits >= 3 && strncmp(p, " - ", 3) == 0 && p[3] != '\0') {
            strncpy(variations[(*count)++], p + 3, 255);
            variations[*count - 1][255] = '\0';
        }
    }

    /* Variation 7: strip " Version" suffix (e.g., "Fire Red Version" → "Fire Red") */
    if (*count < 14) {
        size_t clen = strlen(clean);
        if (clen > 8 && strcasecmp(clean + clen - 8, " Version") == 0) {
            char temp[256];
            strncpy(temp, clean, clen - 8);
            temp[clen - 8] = '\0';
            strncpy(variations[(*count)++], temp, 255);
            variations[*count - 1][255] = '\0';
        }
    }

    /* Variation 8: strip " Edition" suffix */
    if (*count < 14) {
        size_t clen = strlen(clean);
        if (clen > 8 && strcasecmp(clean + clen - 8, " Edition") == 0) {
            char temp[256];
            strncpy(temp, clean, clen - 8);
            temp[clen - 8] = '\0';
            strncpy(variations[(*count)++], temp, 255);
            variations[*count - 1][255] = '\0';
        }
    }

    /* Variation 9: strip all punctuation (periods, apostrophes, colons, hyphens)
     * "Dr. Mario" → "Dr Mario", "Kirby's Adventure" → "Kirbys Adventure" */
    if (*count < 14) {
        char temp[256];
        const char *s = clean;
        char *d = temp;
        char *end = temp + sizeof(temp) - 2;
        int last_space = 0;
        while (*s && d < end) {
            if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                (*s >= '0' && *s <= '9')) {
                *d++ = *s;
                last_space = 0;
            } else {
                /* Replace any punctuation/special char with space */
                if (!last_space && d > temp) {
                    *d++ = ' ';
                    last_space = 1;
                }
            }
            s++;
        }
        /* Trim trailing space */
        if (d > temp && *(d - 1) == ' ') d--;
        *d = '\0';
        if (temp[0] && strcmp(temp, clean) != 0) {
            strncpy(variations[(*count)++], temp, 255);
            variations[*count - 1][255] = '\0';
        }
    }

    /* Variation 10: merge words (remove all spaces) — catches
     * "Duck Tales" → "DuckTales", "Burger Time" → "BurgerTime" */
    if (*count < 14 && strchr(clean, ' ') != NULL) {
        char temp[256];
        char *d = temp;
        char *end_t = temp + sizeof(temp) - 1;
        const char *s = clean;
        while (*s && d < end_t) {
            if (*s != ' ') *d++ = *s;
            s++;
        }
        *d = '\0';
        if (temp[0] && strcmp(temp, clean) != 0) {
            strncpy(variations[(*count)++], temp, 255);
            variations[*count - 1][255] = '\0';
        }
    }

    /* Variation 11: Roman numeral ↔ Arabic number at end of title
     * "Final Fantasy III" → "Final Fantasy 3", "Mega Man 2" → "Mega Man II" */
    if (*count < 14) {
        static const struct { const char *roman; const char *arabic; } numerals[] = {
            {"II", "2"}, {"III", "3"}, {"IV", "4"}, {"V", "5"},
            {"VI", "6"}, {"VII", "7"}, {"VIII", "8"}, {"IX", "9"},
            {"X", "10"}, {"XI", "11"}, {"XII", "12"}, {"XIII", "13"},
            {NULL, NULL}
        };
        size_t clen = strlen(clean);
        int done = 0;
        /* Try Roman → Arabic */
        for (int n = 0; numerals[n].roman && !done; n++) {
            size_t rlen = strlen(numerals[n].roman);
            if (clen > rlen + 1 && clean[clen - rlen - 1] == ' ' &&
                strcmp(clean + clen - rlen, numerals[n].roman) == 0) {
                char temp[256];
                snprintf(temp, sizeof(temp), "%.*s%s",
                    (int)(clen - rlen), clean, numerals[n].arabic);
                strncpy(variations[(*count)++], temp, 255);
                variations[*count - 1][255] = '\0';
                done = 1;
            }
        }
        /* Try Arabic → Roman */
        if (!done) {
            for (int n = 0; numerals[n].arabic && !done; n++) {
                size_t alen = strlen(numerals[n].arabic);
                if (clen > alen + 1 && clean[clen - alen - 1] == ' ' &&
                    strcmp(clean + clen - alen, numerals[n].arabic) == 0) {
                    char temp[256];
                    snprintf(temp, sizeof(temp), "%.*s%s",
                        (int)(clen - alen), clean, numerals[n].roman);
                    strncpy(variations[(*count)++], temp, 255);
                    variations[*count - 1][255] = '\0';
                    done = 1;
                }
            }
        }
    }
}

/* Download a cover image from a URL (IGDB) to local cache */
static char *download_igdb_cover(const char *url, int igdb_id,
                                  const char *cache_dir)
{
    if (!url || !url[0]) return NULL;

    mkdir(cache_dir, 0755);

    char local_path[4096];
    snprintf(local_path, sizeof(local_path), "%s/cover_%d.jpg",
             cache_dir, igdb_id);

    /* Check if already cached */
    struct stat st;
    if (stat(local_path, &st) == 0 && st.st_size > 100) {
        return strdup(local_path);
    }

    /* Retry up to 3 times on transient failures */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) usleep(500000); /* 500ms backoff between retries */

        CURL *dl = curl_easy_init();
        if (!dl) continue;

        FILE *f = fopen(local_path, "wb");
        if (!f) {
            curl_easy_cleanup(dl);
            return NULL;
        }

        curl_easy_setopt(dl, CURLOPT_URL, url);
        curl_easy_setopt(dl, CURLOPT_WRITEDATA, f);
        curl_easy_setopt(dl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(dl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(dl, CURLOPT_FAILONERROR, 1L);

        CURLcode res = curl_easy_perform(dl);
        curl_easy_cleanup(dl);
        fclose(f);

        if (res == CURLE_OK && stat(local_path, &st) == 0 && st.st_size > 100) {
            return strdup(local_path);
        }

        unlink(local_path);

        /* Only retry on transient errors */
        if (res == CURLE_HTTP_RETURNED_ERROR) break; /* 4xx = not found, don't retry */
    }

    return NULL;
}

/* Check if an IGDB platforms string contains a platform matching the target console.
 * Used to verify Phase 3 (no platform filter) results aren't from a wrong console. */
static int platform_matches_console(const char *platforms, int console) {
    if (!platforms || !platforms[0]) return 0;

    const char *keywords[] = {
        [CONSOLE_NES]          = "NES",
        [CONSOLE_SNES]         = "Super Nintendo",
        [CONSOLE_N64]          = "Nintendo 64",
        [CONSOLE_GAMECUBE]     = "GameCube",
        [CONSOLE_WII]          = "Wii",
        [CONSOLE_GB]           = "Game Boy",
        [CONSOLE_GBC]          = "Game Boy Color",
        [CONSOLE_GBA]          = "Game Boy Advance",
        [CONSOLE_PS2]          = "PlayStation 2",
        [CONSOLE_SWITCH]       = "Nintendo Switch",
        [CONSOLE_PS1]          = "PlayStation",
        [CONSOLE_PS3]          = "PlayStation 3",
        [CONSOLE_PS4]          = "PlayStation 4",
        [CONSOLE_PSP]          = "PlayStation Portable",
        [CONSOLE_VITA]         = "PlayStation Vita",
        [CONSOLE_XBOX]         = "Xbox",
        [CONSOLE_XBOX360]      = "Xbox 360",
        [CONSOLE_WIIU]         = "Wii U",
        [CONSOLE_DS]           = "Nintendo DS",
        [CONSOLE_3DS]          = "Nintendo 3DS",
        [CONSOLE_GENESIS]      = "Genesis",
        [CONSOLE_MASTER_SYSTEM]= "Master System",
        [CONSOLE_SATURN]       = "Saturn",
        [CONSOLE_DREAMCAST]    = "Dreamcast",
        [CONSOLE_SEGACD]       = "Sega CD",
        [CONSOLE_ATARI2600]    = "Atari 2600",
        [CONSOLE_TGFX16]       = "TurboGrafx",
        [CONSOLE_32X]          = "32X",
        [CONSOLE_GAMEGEAR]     = "Game Gear",
    };

    if (console < 0 || console >= CONSOLE_COUNT) return 0;

    const char *keyword = keywords[console];
    if (!keyword) return 0;

    /* Special case: "NES" must not match "SNES" or "Super NES" */
    if (console == CONSOLE_NES) {
        const char *p = platforms;
        while ((p = strstr(p, "NES")) != NULL) {
            if (p > platforms && *(p - 1) == 'S') { p += 3; continue; }
            if (p >= platforms + 6 && strncmp(p - 6, "Super ", 6) == 0) { p += 3; continue; }
            return 1;
        }
        return 0;
    }

    /* Special case: "Wii" must not match "Wii U" */
    if (console == CONSOLE_WII) {
        const char *p = platforms;
        while ((p = strstr(p, "Wii")) != NULL) {
            char after = *(p + 3);
            if (after == '\0' || after == ',') return 1;
            if (after == ' ' && *(p + 4) != 'U') return 1;
            p += 3;
        }
        return 0;
    }

    /* Special case: "Game Boy" must not match "Game Boy Color" or "Game Boy Advance" */
    if (console == CONSOLE_GB) {
        const char *p = platforms;
        while ((p = strstr(p, "Game Boy")) != NULL) {
            const char *after = p + 8;
            if (*after == '\0' || *after == ',') return 1;
            if (*after == ' ' && (*(after + 1) == 'C' || *(after + 1) == 'A')) {
                p = after; continue;
            }
            return 1;
        }
        return 0;
    }

    /* Special case: "PlayStation" (PS1) must not match "PlayStation 2/3/4/5/Portable/Vita" */
    if (console == CONSOLE_PS1) {
        const char *p = platforms;
        while ((p = strstr(p, "PlayStation")) != NULL) {
            const char *after = p + 11;
            if (*after == '\0' || *after == ',') return 1;
            p = after;
        }
        return 0;
    }

    /* Special case: "Xbox" must not match "Xbox 360", "Xbox One", "Xbox Series" */
    if (console == CONSOLE_XBOX) {
        const char *p = platforms;
        while ((p = strstr(p, "Xbox")) != NULL) {
            const char *after = p + 4;
            if (*after == '\0' || *after == ',') return 1;
            p = after;
        }
        return 0;
    }

    /* Also try "Mega Drive" for Genesis */
    if (console == CONSOLE_GENESIS) {
        return strstr(platforms, "Genesis") != NULL ||
               strstr(platforms, "Mega Drive") != NULL;
    }

    /* Also try "Mega-CD" for Sega CD */
    if (console == CONSOLE_SEGACD) {
        return strstr(platforms, "Sega CD") != NULL ||
               strstr(platforms, "Mega-CD") != NULL;
    }

    /* Also try "PC Engine" for TurboGrafx */
    if (console == CONSOLE_TGFX16) {
        return strstr(platforms, "TurboGrafx") != NULL ||
               strstr(platforms, "PC Engine") != NULL;
    }

    return strstr(platforms, keyword) != NULL;
}

/* Fetch IGDB metadata for ROMs that don't have it.
 * Uses a 5-phase search strategy per ROM:
 *   Phase 1: search (original + colon variant) with platform filter
 *   Phase 2: search (remaining variations) with platform filter
 *   Phase 3: name ~ (pattern match) with platform filter
 *   Phase 4: search without platform filter + platform validation
 *   Phase 5: name ~ without platform filter + platform validation */
void scanner_fetch_rom_metadata(void) {
    if (!igdb_is_available()) {
        fprintf(stderr, "IGDB: Not initialized, skipping ROM metadata fetch\n");
        return;
    }

    /* First pass: count total ROMs needing metadata */
    int grand_total = 0;
    for (int c = 0; c < CONSOLE_COUNT; c++) {
        int *ids = NULL;
        char **titles = NULL;
        int count = 0;
        if (database_get_roms_without_igdb(c, &ids, &titles, &count) == 0) {
            grand_total += count;
            for (int i = 0; i < count; i++) free(titles[i]);
            free(ids);
            free(titles);
        }
    }

    if (grand_total == 0) return;

    /* Open scrape log file in the root Emulator directory (roms_paths[0]) */
    FILE *logf = NULL;
    if (server_config.roms_path_count > 0 && server_config.roms_paths[0][0]) {
        char logpath[4096];
        snprintf(logpath, sizeof(logpath), "%s/scrape_log.txt", server_config.roms_paths[0]);
        logf = fopen(logpath, "a");
        if (logf) {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char timebuf[64];
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
            fprintf(logf, "\n=== IGDB Scrape Log - %s (%d ROMs) ===\n", timebuf, grand_total);
            fflush(logf);
        }
    }
    /* Enable API-level logging in igdb.c so every request/response is captured */
    igdb_set_log(logf);

    scrape_begin(&igdb_progress, "igdb_fetch", grand_total);
    int found = 0;

    for (int c = 0; c < CONSOLE_COUNT; c++) {
        int *ids = NULL;
        char **titles = NULL;
        int count = 0;

        if (database_get_roms_without_igdb(c, &ids, &titles, &count) != 0)
            continue;

        if (count > 0) {
            int platform_id = igdb_console_to_platform(c);
            const char *console_name = database_console_name(c);
            printf("IGDB: Fetching metadata for %d %s ROMs...\n",
                count, console_name);

            for (int i = 0; i < count; i++) {
                char clean[256];
                clean_rom_title(titles[i], clean, sizeof(clean));

                char variations[14][256];
                int var_count = 0;
                generate_rom_variations(clean, variations, &var_count);

                if (logf) {
                    fprintf(logf, "\nROM: \"%s\" [%s]\n", titles[i], console_name);
                    fprintf(logf, "  Clean: \"%s\"\n", clean);
                    fprintf(logf, "  Variations (%d):", var_count);
                    for (int v = 0; v < var_count; v++)
                        fprintf(logf, " \"%s\"", variations[v]);
                    fprintf(logf, "\n");
                }

                IgdbGame *g = NULL;
                const char *match_method = NULL;
                int queries_tried = 0;

                /* Phase 1: search original + colon variant (indices 0-1) with platform */
                {
                    int phase1_end = var_count < 2 ? var_count : 2;
                    for (int v = 0; v < phase1_end && !g; v++) {
                        g = igdb_search_game(variations[v], platform_id);
                        queries_tried++;
                        if (logf) {
                            fprintf(logf, "  [%d] search \"%s\" plat=%d -> %s\n",
                                queries_tried, variations[v], platform_id,
                                g ? g->name : "MISS");
                        }
                        if (!g) usleep(260000);
                    }
                    if (g) match_method = "search+platform";
                }

                /* Phase 2: search remaining variations with platform */
                if (!g && var_count > 2) {
                    for (int v = 2; v < var_count && !g; v++) {
                        g = igdb_search_game(variations[v], platform_id);
                        queries_tried++;
                        if (logf) {
                            fprintf(logf, "  [%d] search \"%s\" plat=%d -> %s\n",
                                queries_tried, variations[v], platform_id,
                                g ? g->name : "MISS");
                        }
                        if (!g) usleep(260000);
                    }
                    if (g) match_method = "search+platform(var)";
                }

                /* Phase 3: name pattern match with platform filter.
                 * Uses `where name ~ *"title"*` which is a case-insensitive
                 * contains search — catches games the search endpoint misses. */
                if (!g) {
                    /* Try first 3 variations (original, colon, space) */
                    int name_end = var_count < 3 ? var_count : 3;
                    for (int v = 0; v < name_end && !g; v++) {
                        g = igdb_search_game_by_name(variations[v], platform_id);
                        queries_tried++;
                        if (logf) {
                            fprintf(logf, "  [%d] name~ \"%s\" plat=%d -> %s\n",
                                queries_tried, variations[v], platform_id,
                                g ? g->name : "MISS");
                        }
                        if (!g) usleep(260000);
                    }
                    if (g) match_method = "name+platform";
                }

                /* Phase 3.5: word-split name match with platform filter.
                 * Splits title into individual words and requires ALL to be
                 * present in the name.  Catches compound word mismatches:
                 * "Duck Tales" finds "DuckTales" (contains "Duck" + "Tales"). */
                if (!g) {
                    g = igdb_search_game_by_words(variations[0], platform_id);
                    queries_tried++;
                    if (logf) {
                        fprintf(logf, "  [%d] words \"%s\" plat=%d -> %s\n",
                            queries_tried, variations[0], platform_id,
                            g ? g->name : "MISS");
                    }
                    if (!g) usleep(260000);
                    else match_method = "words+platform";
                }

                /* Phase 4: search without platform filter + validation */
                if (!g) {
                    g = igdb_search_game(variations[0], 0);
                    queries_tried++;
                    if (logf) {
                        fprintf(logf, "  [%d] search \"%s\" plat=ANY -> %s\n",
                            queries_tried, variations[0],
                            g ? g->name : "MISS");
                    }
                    if (g && g->platforms && !platform_matches_console(g->platforms, c)) {
                        if (logf) {
                            fprintf(logf, "  [%d] REJECTED (wrong platform: %s)\n",
                                queries_tried, g->platforms);
                        }
                        printf("  IGDB: %s -> %s (wrong platform: %s)\n",
                            titles[i], g->name ? g->name : "?", g->platforms);
                        igdb_free_game(g);
                        g = NULL;
                    }
                    if (!g) usleep(260000);
                    else match_method = "search-noplatform";
                }

                /* Phase 5: name pattern match without platform filter + validation */
                if (!g) {
                    g = igdb_search_game_by_name(variations[0], 0);
                    queries_tried++;
                    if (logf) {
                        fprintf(logf, "  [%d] name~ \"%s\" plat=ANY -> %s\n",
                            queries_tried, variations[0],
                            g ? g->name : "MISS");
                    }
                    if (g && g->platforms && !platform_matches_console(g->platforms, c)) {
                        if (logf) {
                            fprintf(logf, "  [%d] REJECTED (wrong platform: %s)\n",
                                queries_tried, g->platforms);
                        }
                        igdb_free_game(g);
                        g = NULL;
                    }
                    if (!g) usleep(260000);
                    else match_method = "name-noplatform";
                }

                /* Phase 6: word-split search without platform + validation */
                if (!g) {
                    g = igdb_search_game_by_words(variations[0], 0);
                    queries_tried++;
                    if (logf) {
                        fprintf(logf, "  [%d] words \"%s\" plat=ANY -> %s\n",
                            queries_tried, variations[0],
                            g ? g->name : "MISS");
                    }
                    if (g && g->platforms && !platform_matches_console(g->platforms, c)) {
                        if (logf) {
                            fprintf(logf, "  [%d] REJECTED (wrong platform: %s)\n",
                                queries_tried, g->platforms);
                        }
                        igdb_free_game(g);
                        g = NULL;
                    }
                    if (!g) usleep(260000);
                    else match_method = "words-noplatform";
                }

                /* Phase 7: alternative_names search (different regional/marketing titles) */
                if (!g) {
                    g = igdb_search_by_alt_name(variations[0], platform_id);
                    queries_tried++;
                    if (logf) {
                        fprintf(logf, "  [%d] alt_name \"%s\" plat=%d -> %s\n",
                            queries_tried, variations[0], platform_id,
                            g ? g->name : "MISS");
                    }
                    if (!g) usleep(260000);
                    else match_method = "alt_name+platform";
                }

                /* Phase 7b: alternative_names without platform + validation */
                if (!g) {
                    g = igdb_search_by_alt_name(variations[0], 0);
                    queries_tried++;
                    if (logf) {
                        fprintf(logf, "  [%d] alt_name \"%s\" plat=ANY -> %s\n",
                            queries_tried, variations[0],
                            g ? g->name : "MISS");
                    }
                    if (g && g->platforms && !platform_matches_console(g->platforms, c)) {
                        if (logf) {
                            fprintf(logf, "  [%d] REJECTED (wrong platform: %s)\n",
                                queries_tried, g->platforms);
                        }
                        igdb_free_game(g);
                        g = NULL;
                    }
                    if (!g) usleep(260000);
                    else match_method = "alt_name-noplatform";
                }

                if (g) {
                    int has_cover = 0;
                    database_update_rom_metadata(ids[i], g->igdb_id,
                        g->summary, g->developer, g->publisher,
                        g->release_year, g->genres, NULL,
                        g->rating, g->platforms);

                    /* Try cover from search response first */
                    if (g->cover_url) {
                        char *cover = download_igdb_cover(g->cover_url,
                            g->igdb_id, server_config.cache_dir);
                        if (cover) {
                            database_update_rom_cover(ids[i], cover);
                            free(cover);
                            has_cover = 1;
                        }
                    }

                    /* Fallback: targeted cover query if search didn't include it */
                    if (!has_cover && g->igdb_id > 0) {
                        usleep(260000); /* rate limit */
                        char *cover_url = igdb_get_game_cover(g->igdb_id);
                        if (cover_url) {
                            char *cover = download_igdb_cover(cover_url,
                                g->igdb_id, server_config.cache_dir);
                            if (cover) {
                                database_update_rom_cover(ids[i], cover);
                                free(cover);
                                has_cover = 1;
                            }
                            free(cover_url);
                        }
                        if (logf && !has_cover)
                            fprintf(logf, "  COVER FALLBACK: igdb_id=%d -> %s\n",
                                g->igdb_id, has_cover ? "OK" : "FAILED");
                    }

                    printf("  IGDB: %s -> %s (%d) [%s]%s\n",
                        titles[i], g->name ? g->name : "?",
                        g->release_year, match_method,
                        has_cover ? " cover=OK" : "");

                    if (logf) {
                        fprintf(logf, "  RESULT: FOUND \"%s\" (%d) id=%d via %s cover=%s queries=%d\n",
                            g->name ? g->name : "?", g->release_year,
                            g->igdb_id, match_method,
                            has_cover ? "YES" : "NO", queries_tried);
                    }

                    igdb_free_game(g);
                    found++;
                    scrape_update(&igdb_progress, titles[i], 1);
                } else {
                    printf("  IGDB: %s -> NOT FOUND (%d queries)\n", titles[i], queries_tried);
                    if (logf) {
                        fprintf(logf, "  RESULT: NOT FOUND (queries=%d)\n", queries_tried);
                    }
                    database_update_rom_metadata(ids[i], -1,
                        NULL, NULL, NULL, 0, NULL, NULL, 0, NULL);
                    scrape_update(&igdb_progress, titles[i], 0);
                }

                /* Rate limit between ROMs */
                usleep(260000);

                /* Flush log periodically */
                if (logf && (i % 10 == 0)) fflush(logf);
            }
        }

        for (int i = 0; i < count; i++)
            free(titles[i]);
        free(ids);
        free(titles);
    }

    igdb_set_log(NULL);
    if (logf) {
        fprintf(logf, "\n=== Summary: %d/%d found (%.1f%%) ===\n",
            found, grand_total,
            grand_total > 0 ? 100.0 * found / grand_total : 0.0);
        fclose(logf);
    }

    scrape_end(&igdb_progress);
    if (grand_total > 0)
        printf("IGDB: Metadata fetch complete (%d/%d found)\n", found, grand_total);
}

/* Re-fetch IGDB metadata for ALL ROMs (full rescan) */
void scanner_rescan_all_rom_metadata(void) {
    printf("IGDB: Resetting all ROM metadata for full rescan...\n");
    database_reset_rom_igdb();
    scanner_fetch_rom_metadata();
    /* Note: scanner_fetch_rom_metadata sets "igdb_fetch" internally,
     * but the operation is the same — full rescan after reset */
}
