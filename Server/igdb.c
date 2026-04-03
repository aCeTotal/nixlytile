/*
 * Nixly Media Server - IGDB API Client Implementation
 * Uses Twitch OAuth2 for auth, libcurl for HTTP, cJSON for parsing
 *
 * IGDB uses "Apicalypse" query syntax sent as POST body to
 * https://api.igdb.com/v4/{endpoint}
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "igdb.h"
#include "database.h"

#define IGDB_API_BASE "https://api.igdb.com/v4"
#define TWITCH_TOKEN_URL "https://id.twitch.tv/oauth2/token"

static char *client_id = NULL;
static char *client_secret_stored = NULL;
static char *access_token = NULL;
static time_t token_expires = 0;
static CURL *curl_handle = NULL;

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

/* Fetch OAuth2 bearer token from Twitch */
static int igdb_fetch_token(const char *id, const char *secret) {
    if (!curl_handle) return -1;

    char post_data[512];
    snprintf(post_data, sizeof(post_data),
        "client_id=%s&client_secret=%s&grant_type=client_credentials",
        id, secret);

    MemBuffer chunk = {0};
    chunk.data = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl_handle, CURLOPT_URL, TWITCH_TOKEN_URL);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, NULL);

    CURLcode res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        fprintf(stderr, "IGDB: Token request failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        return -1;
    }

    cJSON *json = cJSON_Parse(chunk.data);
    free(chunk.data);
    if (!json) {
        fprintf(stderr, "IGDB: Failed to parse token response\n");
        return -1;
    }

    cJSON *tok = cJSON_GetObjectItem(json, "access_token");
    cJSON *exp = cJSON_GetObjectItem(json, "expires_in");

    if (!tok || !cJSON_IsString(tok)) {
        fprintf(stderr, "IGDB: No access_token in response\n");
        cJSON_Delete(json);
        return -1;
    }

    free(access_token);
    access_token = strdup(tok->valuestring);

    int expires_in = (exp && cJSON_IsNumber(exp)) ? exp->valueint : 3600;
    token_expires = time(NULL) + expires_in - 300; /* Refresh 5 min early */

    cJSON_Delete(json);
    printf("IGDB: Got access token (expires in %d seconds)\n", expires_in);
    return 0;
}

/* Ensure token is valid, refresh if needed */
static int igdb_ensure_token(void) {
    if (access_token && time(NULL) < token_expires)
        return 0;
    if (!client_id || !client_secret_stored) return -1;
    printf("IGDB: Token expired, refreshing...\n");
    return igdb_fetch_token(client_id, client_secret_stored);
}

int igdb_is_available(void) {
    return curl_handle != NULL && client_id != NULL;
}

int igdb_init(const char *id, const char *secret) {
    if (!id || !secret || strlen(id) == 0 || strlen(secret) == 0) {
        fprintf(stderr, "IGDB: No client credentials provided, skipping\n");
        return -1;
    }

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        fprintf(stderr, "IGDB: Failed to init curl\n");
        return -1;
    }

    client_id = strdup(id);
    client_secret_stored = strdup(secret);

    if (igdb_fetch_token(id, secret) != 0) {
        fprintf(stderr, "IGDB: Failed to get initial token\n");
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
        free(client_id);
        client_id = NULL;
        free(client_secret_stored);
        client_secret_stored = NULL;
        return -1;
    }

    printf("IGDB: Initialized successfully\n");
    return 0;
}

void igdb_cleanup(void) {
    if (curl_handle) {
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
    }
    free(client_id);
    free(client_secret_stored);
    free(access_token);
    client_id = NULL;
    client_secret_stored = NULL;
    access_token = NULL;
}

/* Make a POST request to IGDB API endpoint with Apicalypse body */
static char *igdb_request(const char *endpoint, const char *body) {
    if (!curl_handle || igdb_ensure_token() != 0)
        return NULL;

    char url[256];
    snprintf(url, sizeof(url), "%s/%s", IGDB_API_BASE, endpoint);

    /* Build auth headers */
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
    char client_header[256];
    snprintf(client_header, sizeof(client_header), "Client-ID: %s", client_id);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, client_header);
    headers = curl_slist_append(headers, "Content-Type: text/plain");

    MemBuffer chunk = {0};
    chunk.data = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl_handle);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        fprintf(stderr, "IGDB: Request failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        return NULL;
    }

    return chunk.data;
}

static char *get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring)
        return strdup(item->valuestring);
    return NULL;
}

static int get_int(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item))
        return item->valueint;
    return 0;
}

static float get_float(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item))
        return (float)item->valuedouble;
    return 0.0f;
}

/* Extract comma-separated names from an array of objects with "name" fields */
static char *extract_names(cJSON *arr) {
    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
        return NULL;

    size_t buf_size = 512;
    char *buf = malloc(buf_size);
    buf[0] = '\0';
    size_t used = 0;
    int first = 1;

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        const char *name = NULL;
        if (cJSON_IsObject(item)) {
            cJSON *n = cJSON_GetObjectItem(item, "name");
            if (n && cJSON_IsString(n)) name = n->valuestring;
        }
        if (name) {
            if (!first) used += snprintf(buf + used, buf_size - used, ", ");
            used += snprintf(buf + used, buf_size - used, "%s", name);
            first = 0;
        }
    }

    return buf;
}

/* Extract developer and publisher from involved_companies array */
static void extract_companies(cJSON *companies, char **developer, char **publisher) {
    *developer = NULL;
    *publisher = NULL;
    if (!companies || !cJSON_IsArray(companies)) return;

    cJSON *ic;
    cJSON_ArrayForEach(ic, companies) {
        if (!cJSON_IsObject(ic)) continue;

        cJSON *company = cJSON_GetObjectItem(ic, "company");
        if (!company || !cJSON_IsObject(company)) continue;

        cJSON *name = cJSON_GetObjectItem(company, "name");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *is_dev = cJSON_GetObjectItem(ic, "developer");
        cJSON *is_pub = cJSON_GetObjectItem(ic, "publisher");

        if (is_dev && cJSON_IsTrue(is_dev) && !*developer)
            *developer = strdup(name->valuestring);
        if (is_pub && cJSON_IsTrue(is_pub) && !*publisher)
            *publisher = strdup(name->valuestring);
    }
}

int igdb_console_to_platform(int console_type) {
    switch (console_type) {
    case CONSOLE_NES:      return IGDB_PLATFORM_NES;
    case CONSOLE_SNES:     return IGDB_PLATFORM_SNES;
    case CONSOLE_N64:      return IGDB_PLATFORM_N64;
    case CONSOLE_GAMECUBE: return IGDB_PLATFORM_GAMECUBE;
    case CONSOLE_WII:      return IGDB_PLATFORM_WII;
    case CONSOLE_GB:       return IGDB_PLATFORM_GB;
    case CONSOLE_GBC:      return IGDB_PLATFORM_GBC;
    case CONSOLE_GBA:      return IGDB_PLATFORM_GBA;
    default: return 0;
    }
}

/* Normalize a title for comparison: lowercase, &→and, strip punctuation, collapse spaces */
static void normalize_title(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;

    char *d = dst;
    char *end = dst + dst_size - 1;
    int last_space = 1; /* suppress leading spaces */

    while (*src && d < end) {
        unsigned char ch = (unsigned char)*src;

        if (ch == '&') {
            /* & → and */
            if (d + 3 < end) {
                if (!last_space && d > dst) *d++ = ' ';
                *d++ = 'a'; *d++ = 'n'; *d++ = 'd';
                last_space = 0;
            }
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= '0' && ch <= '9')) {
            *d++ = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
            last_space = 0;
        } else if (ch == ' ' || ch == '\'' || ch == '.' || ch == '!' ||
                   ch == ':' || ch == ',' || ch == '-' || ch == '_') {
            if (!last_space && d > dst) {
                *d++ = ' ';
                last_space = 1;
            }
        }
        /* Other punctuation is silently dropped */
        src++;
    }

    /* Trim trailing space */
    if (d > dst && *(d - 1) == ' ') d--;
    *d = '\0';
}

/* Score how well an IGDB result name matches the ROM search title.
 * Returns 0-100; caller should use threshold ~50. */
static int title_match_score(const char *search, const char *result) {
    if (!search || !result) return 0;

    char a[256], b[256];
    normalize_title(search, a, sizeof(a));
    normalize_title(result, b, sizeof(b));

    if (a[0] == '\0' || b[0] == '\0') return 0;

    /* Strip leading "the " from both */
    const char *sa = a, *sb = b;
    if (strncmp(sa, "the ", 4) == 0) sa += 4;
    if (strncmp(sb, "the ", 4) == 0) sb += 4;

    /* Exact match */
    if (strcmp(sa, sb) == 0) return 100;

    size_t la = strlen(sa), lb = strlen(sb);

    /* One contains the other */
    if (la > 0 && lb > 0) {
        if (strstr(sa, sb) || strstr(sb, sa)) {
            size_t shorter = la < lb ? la : lb;
            size_t longer = la > lb ? la : lb;
            /* Score based on length ratio: identical lengths → 95, half → 75 */
            int score = 75 + (int)(20 * shorter / longer);
            return score;
        }
    }

    /* Word overlap scoring */
    /* Tokenize a copy of sa */
    char wa[256], wb[256];
    strncpy(wa, sa, sizeof(wa) - 1); wa[sizeof(wa) - 1] = '\0';
    strncpy(wb, sb, sizeof(wb) - 1); wb[sizeof(wb) - 1] = '\0';

    char *words_a[32], *words_b[32];
    int na = 0, nb = 0;

    for (char *tok = strtok(wa, " "); tok && na < 32; tok = strtok(NULL, " "))
        words_a[na++] = tok;
    for (char *tok = strtok(wb, " "); tok && nb < 32; tok = strtok(NULL, " "))
        words_b[nb++] = tok;

    if (na == 0 || nb == 0) return 0;

    int common = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (strcmp(words_a[i], words_b[j]) == 0) {
                common++;
                break;
            }
        }
    }

    /* Dice coefficient: (2 * common) / (na + nb) scaled to 0-90 */
    int total = na + nb;
    int score = (int)(90.0 * 2.0 * common / total);
    return score;
}

/* Parse a single game object from IGDB JSON into IgdbGame struct */
static IgdbGame *parse_igdb_game(cJSON *game) {
    if (!game) return NULL;

    IgdbGame *g = calloc(1, sizeof(IgdbGame));
    g->igdb_id = get_int(game, "id");
    g->name = get_string(game, "name");
    g->summary = get_string(game, "summary");
    g->rating = get_float(game, "total_rating");

    /* first_release_date is a Unix timestamp */
    cJSON *frd = cJSON_GetObjectItem(game, "first_release_date");
    if (frd && cJSON_IsNumber(frd)) {
        time_t ts = (time_t)frd->valuedouble;
        struct tm *tm = gmtime(&ts);
        if (tm) g->release_year = 1900 + tm->tm_year;
    }

    g->genres = extract_names(cJSON_GetObjectItem(game, "genres"));
    g->platforms = extract_names(cJSON_GetObjectItem(game, "platforms"));
    extract_companies(cJSON_GetObjectItem(game, "involved_companies"),
                      &g->developer, &g->publisher);

    /* Extract cover URL from cover.image_id */
    cJSON *cover = cJSON_GetObjectItem(game, "cover");
    if (cover && cJSON_IsObject(cover)) {
        cJSON *image_id = cJSON_GetObjectItem(cover, "image_id");
        if (image_id && cJSON_IsString(image_id)) {
            char url[512];
            snprintf(url, sizeof(url),
                "https://images.igdb.com/igdb/image/upload/t_cover_big/%s.jpg",
                image_id->valuestring);
            g->cover_url = strdup(url);
        }
    }

    return g;
}

IgdbGame *igdb_search_game(const char *title, int platform_id) {
    if (!title || !title[0]) return NULL;

    /* Escape double quotes by removing them (they break Apicalypse syntax) */
    char safe_title[512];
    const char *s = title;
    char *d = safe_title;
    char *end = safe_title + sizeof(safe_title) - 1;
    while (*s && d < end) {
        if (*s != '"') *d++ = *s;
        s++;
    }
    *d = '\0';

    /* Build Apicalypse query — fetch up to 10 results for scoring */
    char body[1024];
    if (platform_id > 0) {
        snprintf(body, sizeof(body),
            "search \"%s\";\n"
            "fields name,summary,first_release_date,genres.name,platforms.name,"
            "involved_companies.company.name,involved_companies.developer,"
            "involved_companies.publisher,total_rating,cover.image_id;\n"
            "where platforms = (%d);\n"
            "limit 10;",
            safe_title, platform_id);
    } else {
        snprintf(body, sizeof(body),
            "search \"%s\";\n"
            "fields name,summary,first_release_date,genres.name,platforms.name,"
            "involved_companies.company.name,involved_companies.developer,"
            "involved_companies.publisher,total_rating,cover.image_id;\n"
            "limit 10;",
            safe_title);
    }

    char *response = igdb_request("games", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    if (!cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Score each result against the search title and pick the best */
    int best_score = 0;
    cJSON *best_game = NULL;
    int n = cJSON_GetArraySize(json);

    for (int i = 0; i < n; i++) {
        cJSON *game = cJSON_GetArrayItem(json, i);
        if (!game) continue;

        cJSON *name_item = cJSON_GetObjectItem(game, "name");
        if (!name_item || !cJSON_IsString(name_item)) continue;

        int score = title_match_score(title, name_item->valuestring);
        if (score > best_score) {
            best_score = score;
            best_game = game;
        }
    }

    /* Require minimum score of 50 */
    if (best_score < 50 || !best_game) {
        cJSON_Delete(json);
        return NULL;
    }

    IgdbGame *g = parse_igdb_game(best_game);
    cJSON_Delete(json);
    return g;
}

void igdb_free_game(IgdbGame *g) {
    if (!g) return;
    free(g->name);
    free(g->summary);
    free(g->genres);
    free(g->platforms);
    free(g->developer);
    free(g->publisher);
    free(g->cover_url);
    free(g);
}
