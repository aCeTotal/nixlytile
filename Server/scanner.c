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
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>

#include "scanner.h"
#include "database.h"
#include "tmdb.h"
#include "config.h"

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
static int parse_tv_info(const char *filename, char *show_name, int *season, int *episode, int *year) {
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
static void fetch_movie_metadata(int db_id, const char *title) {
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
    } else {
        printf("  TMDB: No match found after %d attempts\n", var_count);
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
static void fetch_episode_metadata(int db_id, const char *show_name, int season, int episode, int year) {
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
        return;
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
        tmdb_free_episode(ep);
    } else {
        tmdb_data.tmdb_id = show->tmdb_id;
        printf("  TMDB: Found show \"%s\", episode not found\n", show->name);
    }

    database_update_tmdb(db_id, &tmdb_data);

    free(tmdb_data.poster_path);
    free(tmdb_data.backdrop_path);
    free(tmdb_data.still_path);
    tmdb_free_tvshow(show);
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

    if (parse_tv_info(filepath, show_name, &season, &episode, &show_year)) {
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

    for (int i = 0; i < count; i++) {
        MediaEntry *e = &entries[i];

        if (e->type == MEDIA_TYPE_MOVIE) {
            fetch_movie_metadata(e->id, e->title);
        } else if (e->type == MEDIA_TYPE_EPISODE && e->show_name) {
            fetch_episode_metadata(e->id, e->show_name, e->season, e->episode, e->year);
        }

        database_free_entry(e);
    }

    free(entries);
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

    for (int i = 0; i < count; i++) {
        MediaEntry *e = &entries[i];

        if (e->type == MEDIA_TYPE_MOVIE) {
            fetch_movie_metadata(e->id, e->title);
        } else if ((e->type == MEDIA_TYPE_EPISODE || e->type == MEDIA_TYPE_TVSHOW) && e->show_name) {
            fetch_episode_metadata(e->id, e->show_name, e->season, e->episode, e->year);
        }

        database_free_entry(e);
    }

    free(entries);
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

    for (int i = 0; i < count; i++) {
        TmdbTvShow *show = tmdb_get_show_status(ids[i]);
        if (show) {
            database_update_show_status(ids[i], show->status, show->next_episode_date,
                                        show->number_of_seasons, show->number_of_episodes,
                                        show->episode_run_time);
            printf("  Show %d: %s (%d seasons, %d episodes, %d min/ep, next: %s)\n",
                   ids[i],
                   show->status ? show->status : "?",
                   show->number_of_seasons, show->number_of_episodes,
                   show->episode_run_time,
                   show->next_episode_date ? show->next_episode_date : "none");
            tmdb_free_tvshow(show);
        }
    }

    free(ids);
    printf("Show status refresh complete\n");
}
