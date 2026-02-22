/*
 * Nixly Media Server - TMDB API Client Implementation
 * Uses libcurl for HTTP requests and cJSON for parsing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include "tmdb.h"
#include "config.h"
#include <cjson/cJSON.h>

#define TMDB_API_BASE "https://api.themoviedb.org/3"
#define TMDB_IMAGE_BASE "https://image.tmdb.org/t/p"

static char *api_key = NULL;
static char *language = NULL;
static CURL *curl = NULL;

typedef struct {
    char *data;
    size_t size;
} MemBuffer;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemBuffer *mem = (MemBuffer *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

static char *url_encode(const char *str) {
    if (!curl) return strdup(str);
    char *encoded = curl_easy_escape(curl, str, 0);
    char *result = strdup(encoded);
    curl_free(encoded);
    return result;
}

static char *tmdb_request_lang(const char *endpoint, const char *lang) {
    if (!curl || !api_key) return NULL;

    char url[2048];
    char sep = strchr(endpoint, '?') ? '&' : '?';
    snprintf(url, sizeof(url), "%s%s%capi_key=%s&language=%s",
             TMDB_API_BASE, endpoint, sep, api_key, lang);

    MemBuffer chunk = {0};
    chunk.data = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "TMDB request failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        return NULL;
    }

    return chunk.data;
}

static char *tmdb_request(const char *endpoint) {
    return tmdb_request_lang(endpoint, language ? language : "en-US");
}

/* Fetch overview with English fallback when configured language returns empty */
static char *get_overview_fallback(cJSON *json, const char *endpoint) {
    char *overview = get_string(json, "overview");
    if (overview && overview[0]) return overview;

    /* Already English or no language set - no fallback needed */
    if (!language || strcmp(language, "en-US") == 0) return overview;

    free(overview);
    char *response = tmdb_request_lang(endpoint, "en-US");
    if (!response) return NULL;

    cJSON *en_json = cJSON_Parse(response);
    free(response);
    if (!en_json) return NULL;

    overview = get_string(en_json, "overview");
    cJSON_Delete(en_json);
    return overview;
}

int tmdb_init(const char *key, const char *lang) {
    if (!key || strlen(key) == 0) {
        fprintf(stderr, "TMDB: No API key provided\n");
        return -1;
    }

    api_key = strdup(key);
    language = lang ? strdup(lang) : strdup("en-US");

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (!curl) {
        fprintf(stderr, "TMDB: Failed to init curl\n");
        return -1;
    }

    printf("TMDB: Initialized with language %s\n", language);
    return 0;
}

void tmdb_cleanup(void) {
    if (curl) {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    curl_global_cleanup();
    free(api_key);
    free(language);
    api_key = NULL;
    language = NULL;
}

static char *get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        return strdup(item->valuestring);
    }
    return NULL;
}

static int get_int(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return 0;
}

static float get_float(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return 0.0f;
}

static char *get_genres(cJSON *obj) {
    cJSON *genres = cJSON_GetObjectItem(obj, "genres");
    if (!genres && !cJSON_IsArray(genres)) {
        /* Try genre_ids for search results */
        genres = cJSON_GetObjectItem(obj, "genre_ids");
        if (!genres) return NULL;
    }

    size_t buf_size = 256;
    char *buf = malloc(buf_size);
    buf[0] = '\0';
    size_t used = 0;

    cJSON *genre;
    int first = 1;
    cJSON_ArrayForEach(genre, genres) {
        const char *name = NULL;
        if (cJSON_IsObject(genre)) {
            cJSON *n = cJSON_GetObjectItem(genre, "name");
            if (n && cJSON_IsString(n)) name = n->valuestring;
        }
        if (name) {
            if (!first) {
                used += snprintf(buf + used, buf_size - used, ", ");
            }
            used += snprintf(buf + used, buf_size - used, "%s", name);
            first = 0;
        }
    }

    return buf;
}

static int extract_year(const char *date) {
    if (!date || strlen(date) < 4) return 0;
    return atoi(date);
}

TmdbMovie *tmdb_search_movie(const char *title, int year) {
    char *encoded = url_encode(title);
    char endpoint[512];

    /* If we have a year, use TMDB's year filter for precise matching */
    if (year > 0) {
        snprintf(endpoint, sizeof(endpoint), "/search/movie?query=%s&primary_release_year=%d", encoded, year);
    } else {
        snprintf(endpoint, sizeof(endpoint), "/search/movie?query=%s", encoded);
    }
    free(encoded);

    char *response = tmdb_request(endpoint);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    cJSON *results = cJSON_GetObjectItem(json, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Find best matching movie by year */
    cJSON *best_match = NULL;
    int best_score = -1;
    int result_count = cJSON_GetArraySize(results);

    for (int i = 0; i < result_count; i++) {
        cJSON *movie = cJSON_GetArrayItem(results, i);
        char *release = get_string(movie, "release_date");
        int movie_year = extract_year(release);
        free(release);

        int score = 0;

        /* Exact year match gets highest priority */
        if (year > 0 && movie_year == year) {
            score = 1000;
        } else if (year > 0 && movie_year > 0) {
            /* Penalize by year difference */
            int diff = abs(movie_year - year);
            if (diff <= 1) {
                score = 100 - diff * 10;  /* Within 1 year */
            } else {
                score = 50 - diff;  /* Further away */
            }
        } else {
            /* No year specified, prefer earlier results (usually more popular) */
            score = 50 - i;
        }

        if (score > best_score) {
            best_score = score;
            best_match = movie;
        }
    }

    if (!best_match) {
        best_match = cJSON_GetArrayItem(results, 0);
    }

    int tmdb_id = get_int(best_match, "id");

    /* Log what we found for debugging */
    char *match_date = get_string(best_match, "release_date");
    char *match_title = get_string(best_match, "title");
    printf("  TMDB: Selected \"%s\" (%s) from %d results (wanted year: %d)\n",
           match_title ? match_title : "?", match_date ? match_date : "?",
           result_count, year);
    free(match_date);
    free(match_title);

    cJSON_Delete(json);

    /* Fetch full movie details */
    snprintf(endpoint, sizeof(endpoint), "/movie/%d", tmdb_id);
    response = tmdb_request(endpoint);
    if (!response) return NULL;

    json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    TmdbMovie *m = calloc(1, sizeof(TmdbMovie));
    m->tmdb_id = get_int(json, "id");
    m->title = get_string(json, "title");
    m->original_title = get_string(json, "original_title");
    m->overview = get_overview_fallback(json, endpoint);
    m->poster_path = get_string(json, "poster_path");
    m->backdrop_path = get_string(json, "backdrop_path");
    m->release_date = get_string(json, "release_date");
    m->year = extract_year(m->release_date);
    m->rating = get_float(json, "vote_average");
    m->vote_count = get_int(json, "vote_count");
    m->genres = get_genres(json);
    m->runtime = get_int(json, "runtime");

    cJSON_Delete(json);
    return m;
}

TmdbTvShow *tmdb_search_tvshow_ex(const char *title, int api_filter_year, int wanted_year) {
    char *encoded = url_encode(title);
    char endpoint[512];

    /* If we have an API filter year, use TMDB's first_air_date_year filter */
    if (api_filter_year > 0) {
        snprintf(endpoint, sizeof(endpoint), "/search/tv?query=%s&first_air_date_year=%d", encoded, api_filter_year);
    } else {
        snprintf(endpoint, sizeof(endpoint), "/search/tv?query=%s", encoded);
    }
    free(encoded);

    char *response = tmdb_request(endpoint);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    cJSON *results = cJSON_GetObjectItem(json, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Find best matching show by wanted_year (not api_filter_year) */
    cJSON *best_match = NULL;
    int best_score = -1;
    int result_count = cJSON_GetArraySize(results);

    for (int i = 0; i < result_count; i++) {
        cJSON *show = cJSON_GetArrayItem(results, i);
        char *first_air = get_string(show, "first_air_date");
        int show_year = extract_year(first_air);
        free(first_air);

        int score = 0;

        /* Exact year match gets highest priority */
        if (wanted_year > 0 && show_year == wanted_year) {
            score = 1000;
        } else if (wanted_year > 0 && show_year > 0) {
            /* Penalize by year difference */
            int diff = abs(show_year - wanted_year);
            if (diff <= 1) {
                score = 100 - diff * 10;  /* Within 1 year */
            } else {
                score = 50 - diff;  /* Further away */
            }
        } else {
            /* No year specified, prefer earlier results (usually more popular) */
            score = 50 - i;
        }

        if (score > best_score) {
            best_score = score;
            best_match = show;
        }
    }

    if (!best_match) {
        best_match = cJSON_GetArrayItem(results, 0);
    }

    int tmdb_id = get_int(best_match, "id");

    /* Log what we found for debugging */
    char *match_date = get_string(best_match, "first_air_date");
    char *match_name = get_string(best_match, "name");
    printf("  TMDB: Selected \"%s\" (%s) from %d results (wanted year: %d)\n",
           match_name ? match_name : "?", match_date ? match_date : "?",
           result_count, wanted_year);
    free(match_date);
    free(match_name);

    cJSON_Delete(json);

    /* Fetch full TV show details */
    snprintf(endpoint, sizeof(endpoint), "/tv/%d", tmdb_id);
    response = tmdb_request(endpoint);
    if (!response) return NULL;

    json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    TmdbTvShow *t = calloc(1, sizeof(TmdbTvShow));
    t->tmdb_id = get_int(json, "id");
    t->name = get_string(json, "name");
    t->original_name = get_string(json, "original_name");
    t->overview = get_overview_fallback(json, endpoint);
    t->poster_path = get_string(json, "poster_path");
    t->backdrop_path = get_string(json, "backdrop_path");
    t->first_air_date = get_string(json, "first_air_date");
    t->year = extract_year(t->first_air_date);
    t->rating = get_float(json, "vote_average");
    t->vote_count = get_int(json, "vote_count");
    t->genres = get_genres(json);
    t->number_of_seasons = get_int(json, "number_of_seasons");
    t->number_of_episodes = get_int(json, "number_of_episodes");

    /* episode_run_time is an array - take the first value */
    cJSON *runtimes = cJSON_GetObjectItem(json, "episode_run_time");
    if (runtimes && cJSON_IsArray(runtimes) && cJSON_GetArraySize(runtimes) > 0) {
        cJSON *first = cJSON_GetArrayItem(runtimes, 0);
        if (first && cJSON_IsNumber(first)) {
            t->episode_run_time = first->valueint;
        }
    }

    /* Fallback: use last_episode_to_air.runtime if episode_run_time array was empty */
    if (t->episode_run_time == 0) {
        cJSON *last_ep = cJSON_GetObjectItem(json, "last_episode_to_air");
        if (last_ep && cJSON_IsObject(last_ep)) {
            t->episode_run_time = get_int(last_ep, "runtime");
        }
    }

    /* Show status: "Returning Series", "Ended", "Canceled", etc. */
    t->status = get_string(json, "status");

    /* Next episode to air */
    cJSON *next_ep = cJSON_GetObjectItem(json, "next_episode_to_air");
    if (next_ep && cJSON_IsObject(next_ep)) {
        t->next_episode_date = get_string(next_ep, "air_date");
    }

    cJSON_Delete(json);
    return t;
}

/* Lightweight fetch: get just show status and next episode date */
TmdbTvShow *tmdb_get_show_status(int tv_id) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/tv/%d", tv_id);

    char *response = tmdb_request(endpoint);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    TmdbTvShow *t = calloc(1, sizeof(TmdbTvShow));
    t->tmdb_id = get_int(json, "id");
    t->status = get_string(json, "status");
    t->number_of_seasons = get_int(json, "number_of_seasons");
    t->number_of_episodes = get_int(json, "number_of_episodes");

    /* episode_run_time array */
    cJSON *runtimes = cJSON_GetObjectItem(json, "episode_run_time");
    if (runtimes && cJSON_IsArray(runtimes) && cJSON_GetArraySize(runtimes) > 0) {
        cJSON *first = cJSON_GetArrayItem(runtimes, 0);
        if (first && cJSON_IsNumber(first)) {
            t->episode_run_time = first->valueint;
        }
    }

    /* Fallback: use last_episode_to_air.runtime */
    if (t->episode_run_time == 0) {
        cJSON *last_ep = cJSON_GetObjectItem(json, "last_episode_to_air");
        if (last_ep && cJSON_IsObject(last_ep)) {
            t->episode_run_time = get_int(last_ep, "runtime");
        }
    }

    cJSON *next_ep = cJSON_GetObjectItem(json, "next_episode_to_air");
    if (next_ep && cJSON_IsObject(next_ep)) {
        t->next_episode_date = get_string(next_ep, "air_date");
    }

    cJSON_Delete(json);
    return t;
}

/* Wrapper for backwards compatibility - uses same year for API filter and scoring */
TmdbTvShow *tmdb_search_tvshow(const char *title, int year) {
    return tmdb_search_tvshow_ex(title, year, year);
}

TmdbEpisode *tmdb_get_episode(int tv_id, int season, int episode) {
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/tv/%d/season/%d/episode/%d", tv_id, season, episode);

    char *response = tmdb_request(endpoint);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    TmdbEpisode *e = calloc(1, sizeof(TmdbEpisode));
    e->episode_number = get_int(json, "episode_number");
    e->season_number = get_int(json, "season_number");
    e->name = get_string(json, "name");
    e->overview = get_overview_fallback(json, endpoint);
    e->still_path = get_string(json, "still_path");
    e->air_date = get_string(json, "air_date");
    e->rating = get_float(json, "vote_average");
    e->runtime = get_int(json, "runtime");

    cJSON_Delete(json);
    return e;
}

char *tmdb_image_url(const char *path, const char *size) {
    if (!path) return NULL;
    char *url = malloc(256);
    snprintf(url, 256, "%s/%s%s", TMDB_IMAGE_BASE, size ? size : "w500", path);
    return url;
}

static size_t file_write_callback(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

char *tmdb_download_image(const char *tmdb_path, const char *size, const char *cache_dir) {
    if (!tmdb_path || !curl) return NULL;

    /* Create cache directory if needed */
    mkdir(cache_dir, 0755);

    /* Build local filename from tmdb path */
    const char *filename = strrchr(tmdb_path, '/');
    filename = filename ? filename + 1 : tmdb_path;

    char local_path[MAX_PATH_LEN];
    snprintf(local_path, sizeof(local_path), "%s/%s_%s", cache_dir, size ? size : "w500", filename);

    /* Check if already cached */
    struct stat st;
    if (stat(local_path, &st) == 0 && st.st_size > 0) {
        return strdup(local_path);
    }

    /* Download */
    char *url = tmdb_image_url(tmdb_path, size);
    if (!url) return NULL;

    FILE *f = fopen(local_path, "wb");
    if (!f) {
        free(url);
        return NULL;
    }

    CURL *dl = curl_easy_init();
    curl_easy_setopt(dl, CURLOPT_URL, url);
    curl_easy_setopt(dl, CURLOPT_WRITEFUNCTION, file_write_callback);
    curl_easy_setopt(dl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(dl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(dl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(dl);
    curl_easy_cleanup(dl);
    fclose(f);
    free(url);

    if (res != CURLE_OK) {
        unlink(local_path);
        return NULL;
    }

    return strdup(local_path);
}

void tmdb_free_movie(TmdbMovie *m) {
    if (!m) return;
    free(m->title);
    free(m->original_title);
    free(m->overview);
    free(m->poster_path);
    free(m->backdrop_path);
    free(m->release_date);
    free(m->genres);
    free(m);
}

void tmdb_free_tvshow(TmdbTvShow *t) {
    if (!t) return;
    free(t->name);
    free(t->original_name);
    free(t->overview);
    free(t->poster_path);
    free(t->backdrop_path);
    free(t->first_air_date);
    free(t->genres);
    free(t->status);
    free(t->next_episode_date);
    free(t);
}

void tmdb_free_episode(TmdbEpisode *e) {
    if (!e) return;
    free(e->name);
    free(e->overview);
    free(e->still_path);
    free(e->air_date);
    free(e);
}
