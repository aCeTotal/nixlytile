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
#include <ctype.h>
#include <pthread.h>
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

/* Protect curl_handle from concurrent access (server is multi-threaded) */
static pthread_mutex_t igdb_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Optional log file for API-level request/response tracking.
 * Set via igdb_set_log() before scraping. */
static FILE *igdb_logfile = NULL;

void igdb_set_log(FILE *f) { igdb_logfile = f; }

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

    /* Reset all options so stale state from API calls doesn't bleed in */
    curl_easy_reset(curl_handle);
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

/* Make a POST request to IGDB API endpoint with Apicalypse body.
 * Thread-safe: serializes access to the shared curl_handle.
 * Retries on 429 rate limiting with exponential backoff. */
static char *igdb_request(const char *endpoint, const char *body) {
    pthread_mutex_lock(&igdb_mutex);

    if (!curl_handle || igdb_ensure_token() != 0) {
        pthread_mutex_unlock(&igdb_mutex);
        return NULL;
    }

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

    /* Log the request body to API log if enabled */
    if (igdb_logfile) {
        fprintf(igdb_logfile, "    API %s: %s\n", endpoint, body);
    }

    /* Retry loop for rate limiting (429) */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0) {
            /* Exponential backoff: 1s, 2s, 4s */
            int backoff_ms = 1000 * (1 << (attempt - 1));
            fprintf(stderr, "IGDB: Retry %d after %dms backoff\n", attempt, backoff_ms);
            pthread_mutex_unlock(&igdb_mutex);
            usleep(backoff_ms * 1000);
            pthread_mutex_lock(&igdb_mutex);
            if (igdb_ensure_token() != 0) {
                pthread_mutex_unlock(&igdb_mutex);
                curl_slist_free_all(headers);
                return NULL;
            }
        }

        MemBuffer chunk = {0};
        chunk.data = malloc(1);
        chunk.size = 0;

        curl_easy_reset(curl_handle);
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &chunk);
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 15L);

        CURLcode res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            fprintf(stderr, "IGDB: Request failed: %s\n", curl_easy_strerror(res));
            if (igdb_logfile) fprintf(igdb_logfile, "    -> CURL ERROR: %s\n", curl_easy_strerror(res));
            free(chunk.data);

            /* Recoverable CURL errors: recreate handle and retry */
            if (res == CURLE_FAILED_INIT || res == CURLE_COULDNT_CONNECT ||
                res == CURLE_OPERATION_TIMEDOUT) {
                fprintf(stderr, "IGDB: Recreating curl handle after %s\n",
                    curl_easy_strerror(res));
                curl_easy_cleanup(curl_handle);
                curl_handle = curl_easy_init();
                if (curl_handle) {
                    /* Rebuild headers (old slist still valid) */
                    int backoff_ms = 500 * (1 << attempt);
                    pthread_mutex_unlock(&igdb_mutex);
                    usleep(backoff_ms * 1000);
                    pthread_mutex_lock(&igdb_mutex);
                    continue; /* retry with fresh handle */
                }
            }

            curl_slist_free_all(headers);
            pthread_mutex_unlock(&igdb_mutex);
            return NULL;
        }

        long http_code = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200) {
            curl_slist_free_all(headers);
            pthread_mutex_unlock(&igdb_mutex);
            return chunk.data;
        }

        /* Non-200 response */
        char preview[256];
        size_t plen = chunk.size < sizeof(preview) - 1 ? chunk.size : sizeof(preview) - 1;
        memcpy(preview, chunk.data, plen);
        preview[plen] = '\0';

        fprintf(stderr, "IGDB: HTTP %ld from %s: %s\n", http_code, endpoint, preview);
        if (igdb_logfile) fprintf(igdb_logfile, "    -> HTTP %ld: %s\n", http_code, preview);
        free(chunk.data);

        if (http_code == 401) {
            fprintf(stderr, "IGDB: 401 Unauthorized, invalidating token\n");
            token_expires = 0;
            /* Retry will re-fetch token */
            continue;
        } else if (http_code == 429) {
            fprintf(stderr, "IGDB: Rate limited (429), backing off...\n");
            continue; /* retry with backoff */
        } else {
            /* Other error (400, 500, etc.) — don't retry */
            break;
        }
    }

    curl_slist_free_all(headers);
    pthread_mutex_unlock(&igdb_mutex);
    return NULL;
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
    case CONSOLE_PS2:      return IGDB_PLATFORM_PS2;
    case CONSOLE_SWITCH:   return IGDB_PLATFORM_SWITCH;
    case CONSOLE_PS1:      return IGDB_PLATFORM_PS1;
    case CONSOLE_PS3:      return IGDB_PLATFORM_PS3;
    case CONSOLE_PS4:      return IGDB_PLATFORM_PS4;
    case CONSOLE_PSP:      return IGDB_PLATFORM_PSP;
    case CONSOLE_VITA:     return IGDB_PLATFORM_VITA;
    case CONSOLE_XBOX:     return IGDB_PLATFORM_XBOX;
    case CONSOLE_XBOX360:  return IGDB_PLATFORM_XBOX360;
    case CONSOLE_WIIU:     return IGDB_PLATFORM_WIIU;
    case CONSOLE_DS:       return IGDB_PLATFORM_DS;
    case CONSOLE_3DS:      return IGDB_PLATFORM_3DS;
    case CONSOLE_GENESIS:  return IGDB_PLATFORM_GENESIS;
    case CONSOLE_MASTER_SYSTEM: return IGDB_PLATFORM_SMS;
    case CONSOLE_SATURN:   return IGDB_PLATFORM_SATURN;
    case CONSOLE_DREAMCAST: return IGDB_PLATFORM_DREAMCAST;
    case CONSOLE_SEGACD:   return IGDB_PLATFORM_SEGACD;
    case CONSOLE_ATARI2600: return IGDB_PLATFORM_ATARI2600;
    case CONSOLE_TGFX16:   return IGDB_PLATFORM_TGFX16;
    case CONSOLE_32X:      return IGDB_PLATFORM_32X;
    case CONSOLE_GAMEGEAR: return IGDB_PLATFORM_GAMEGEAR;
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
        } else if (ch == '\'') {
            /* Possessive 's: drop both apostrophe and trailing s
             * "Kirby's" → "kirby", not "kirby s" */
            unsigned char next = (unsigned char)*(src + 1);
            if ((next == 's' || next == 'S') &&
                (!*(src + 2) || !isalnum((unsigned char)*(src + 2)))) {
                src++; /* skip the 's' too */
            }
            if (!last_space && d > dst) { *d++ = ' '; last_space = 1; }
        } else if (ch == ' ' || ch == '.' || ch == '!' ||
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

    /* Compute space-stripped versions for fuzzy comparison.
     * Catches "Mega Man" vs "MegaMan", "Q*bert" vs "Q-bert",
     * "Duck Tales" vs "DuckTales", etc. */
    char sa_ns[256], sb_ns[256];
    {
        char *da = sa_ns, *db = sb_ns;
        for (const char *p = sa; *p; p++) if (*p != ' ') *da++ = *p;
        *da = '\0';
        for (const char *p = sb; *p; p++) if (*p != ' ') *db++ = *p;
        *db = '\0';
    }

    /* Space-stripped exact match */
    if (sa_ns[0] && sb_ns[0] && strcmp(sa_ns, sb_ns) == 0) return 95;

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

    /* Space-stripped substring check */
    {
        size_t la_ns = strlen(sa_ns), lb_ns = strlen(sb_ns);
        if (la_ns > 0 && lb_ns > 0 && (strstr(sa_ns, sb_ns) || strstr(sb_ns, sa_ns))) {
            size_t shorter = la_ns < lb_ns ? la_ns : lb_ns;
            size_t longer = la_ns > lb_ns ? la_ns : lb_ns;
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

/* Fetch cover image_id from the covers endpoint by cover table ID.
 * Returns constructed cover URL (caller frees) or NULL. */
static char *igdb_fetch_cover_url(int cover_id) {
    if (cover_id <= 0) return NULL;

    char body[128];
    snprintf(body, sizeof(body), "fields image_id;\nwhere id = %d;\nlimit 1;", cover_id);

    char *response = igdb_request("covers", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json || !cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    char *result = NULL;
    cJSON *item = cJSON_GetArrayItem(json, 0);
    if (item) {
        cJSON *image_id = cJSON_GetObjectItem(item, "image_id");
        if (image_id && cJSON_IsString(image_id)) {
            char url[512];
            snprintf(url, sizeof(url),
                "https://images.igdb.com/igdb/image/upload/t_cover_big/%s.jpg",
                image_id->valuestring);
            result = strdup(url);
        }
    }

    cJSON_Delete(json);
    return result;
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

    /* Extract cover URL from cover.image_id.
     * IGDB may return cover as an expanded object {id, image_id} or as a
     * bare integer (the cover-table row ID).  Handle both. */
    cJSON *cover = cJSON_GetObjectItem(game, "cover");
    if (cover) {
        if (cJSON_IsObject(cover)) {
            cJSON *image_id = cJSON_GetObjectItem(cover, "image_id");
            if (image_id && cJSON_IsString(image_id)) {
                char url[512];
                snprintf(url, sizeof(url),
                    "https://images.igdb.com/igdb/image/upload/t_cover_big/%s.jpg",
                    image_id->valuestring);
                g->cover_url = strdup(url);
            }
        } else if (cJSON_IsNumber(cover)) {
            /* cover is integer ID — query covers endpoint */
            g->cover_url = igdb_fetch_cover_url(cover->valueint);
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

    printf("IGDB: Searching for \"%s\" (platform %d)\n", safe_title, platform_id);

    char *response = igdb_request("games", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    if (!cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Check if IGDB returned an error response (array of error objects) */
    cJSON *first = cJSON_GetArrayItem(json, 0);
    if (first) {
        cJSON *status = cJSON_GetObjectItem(first, "status");
        if (status && cJSON_IsNumber(status)) {
            cJSON *errtitle = cJSON_GetObjectItem(first, "title");
            cJSON *errcause = cJSON_GetObjectItem(first, "cause");
            fprintf(stderr, "IGDB: API error %d: %s%s%s\n",
                status->valueint,
                (errtitle && cJSON_IsString(errtitle)) ? errtitle->valuestring : "unknown",
                (errcause && cJSON_IsString(errcause)) ? " — " : "",
                (errcause && cJSON_IsString(errcause)) ? errcause->valuestring : "");
            cJSON_Delete(json);
            return NULL;
        }
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

    /* Require minimum score of 40 (lenient to handle title variations) */
    if (best_score < 40 || !best_game) {
        if (igdb_logfile) {
            if (best_game) {
                cJSON *bn = cJSON_GetObjectItem(best_game, "name");
                fprintf(igdb_logfile, "    -> %d results, best: \"%s\" score=%d (< 40, rejected)\n",
                    n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
            } else {
                fprintf(igdb_logfile, "    -> %d results, none matched\n", n);
            }
        }
        if (best_game) {
            cJSON *bn = cJSON_GetObjectItem(best_game, "name");
            fprintf(stderr, "IGDB: No match for \"%s\" — best candidate \"%s\" score %d (< 40)\n",
                title,
                (bn && cJSON_IsString(bn)) ? bn->valuestring : "?",
                best_score);
        }
        cJSON_Delete(json);
        return NULL;
    }

    if (igdb_logfile) {
        cJSON *bn = cJSON_GetObjectItem(best_game, "name");
        fprintf(igdb_logfile, "    -> %d results, matched: \"%s\" score=%d\n",
            n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
    }

    IgdbGame *g = parse_igdb_game(best_game);
    cJSON_Delete(json);
    return g;
}

IgdbGame *igdb_search_game_by_name(const char *title, int platform_id) {
    if (!title || !title[0]) return NULL;

    /* Build a safe query string: remove characters that break Apicalypse */
    char safe[512];
    const char *s = title;
    char *d = safe;
    char *end = safe + sizeof(safe) - 1;
    while (*s && d < end) {
        if (*s != '"' && *s != '\\' && *s != '*')
            *d++ = *s;
        s++;
    }
    *d = '\0';
    if (safe[0] == '\0') return NULL;

    char body[1024];
    if (platform_id > 0) {
        snprintf(body, sizeof(body),
            "fields name,summary,first_release_date,genres.name,platforms.name,"
            "involved_companies.company.name,involved_companies.developer,"
            "involved_companies.publisher,total_rating,cover.image_id;\n"
            "where name ~ *\"%s\"* & platforms = (%d);\n"
            "limit 10;",
            safe, platform_id);
    } else {
        snprintf(body, sizeof(body),
            "fields name,summary,first_release_date,genres.name,platforms.name,"
            "involved_companies.company.name,involved_companies.developer,"
            "involved_companies.publisher,total_rating,cover.image_id;\n"
            "where name ~ *\"%s\"*;\n"
            "limit 10;",
            safe);
    }

    printf("IGDB: Name search for \"%s\" (platform %d)\n", safe, platform_id);

    char *response = igdb_request("games", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) return NULL;

    if (!cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Check for error objects */
    cJSON *first = cJSON_GetArrayItem(json, 0);
    if (first && cJSON_GetObjectItem(first, "status")) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Score each result same as igdb_search_game */
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

    if (best_score < 40 || !best_game) {
        if (igdb_logfile) {
            if (best_game) {
                cJSON *bn = cJSON_GetObjectItem(best_game, "name");
                fprintf(igdb_logfile, "    -> %d results, best: \"%s\" score=%d (< 40, rejected)\n",
                    n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
            } else {
                fprintf(igdb_logfile, "    -> %d results, none scored\n", n);
            }
        }
        cJSON_Delete(json);
        return NULL;
    }

    if (igdb_logfile) {
        cJSON *bn = cJSON_GetObjectItem(best_game, "name");
        fprintf(igdb_logfile, "    -> %d results, matched: \"%s\" score=%d\n",
            n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
    }

    IgdbGame *g = parse_igdb_game(best_game);
    cJSON_Delete(json);
    return g;
}

/* Helper: build a word-AND query body, search IGDB, and score results.
 * terms[] contains the search terms, nterms is the count.
 * Returns best match or NULL. */
static IgdbGame *igdb_words_query(const char *title, const char *terms[],
                                   int nterms, int platform_id) {
    if (nterms < 2) return NULL;

    char body[1024];
    int pos = 0;
    pos += snprintf(body + pos, sizeof(body) - pos,
        "fields name,summary,first_release_date,genres.name,platforms.name,"
        "involved_companies.company.name,involved_companies.developer,"
        "involved_companies.publisher,total_rating,cover.image_id;\n"
        "where ");
    for (int i = 0; i < nterms; i++) {
        if (i > 0) pos += snprintf(body + pos, sizeof(body) - pos, " & ");
        pos += snprintf(body + pos, sizeof(body) - pos, "name ~ *\"%s\"*", terms[i]);
    }
    if (platform_id > 0)
        pos += snprintf(body + pos, sizeof(body) - pos, " & platforms = (%d)", platform_id);
    pos += snprintf(body + pos, sizeof(body) - pos, ";\nlimit 10;");

    char *response = igdb_request("games", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json || !cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }
    cJSON *first = cJSON_GetArrayItem(json, 0);
    if (first && cJSON_GetObjectItem(first, "status")) {
        cJSON_Delete(json);
        return NULL;
    }

    int best_score = 0;
    cJSON *best_game = NULL;
    int n = cJSON_GetArraySize(json);

    for (int i = 0; i < n; i++) {
        cJSON *game = cJSON_GetArrayItem(json, i);
        if (!game) continue;
        cJSON *name_item = cJSON_GetObjectItem(game, "name");
        if (!name_item || !cJSON_IsString(name_item)) continue;
        int score = title_match_score(title, name_item->valuestring);
        if (score > best_score) { best_score = score; best_game = game; }
    }

    if (best_score < 40 || !best_game) {
        if (igdb_logfile) {
            if (best_game) {
                cJSON *bn = cJSON_GetObjectItem(best_game, "name");
                fprintf(igdb_logfile, "    -> %d results, best: \"%s\" score=%d (< 40, rejected)\n",
                    n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
            } else {
                fprintf(igdb_logfile, "    -> %d results, none scored\n", n);
            }
        }
        cJSON_Delete(json);
        return NULL;
    }

    if (igdb_logfile) {
        cJSON *bn = cJSON_GetObjectItem(best_game, "name");
        fprintf(igdb_logfile, "    -> %d results, matched: \"%s\" score=%d\n",
            n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
    }

    IgdbGame *g = parse_igdb_game(best_game);
    cJSON_Delete(json);
    return g;
}

static int is_stop_word(const char *word) {
    static const char *stop_words[] = {
        "the", "and", "for", "from", "with", "into", "over",
        "its", "his", "her", "that", "this", "not", NULL
    };
    for (int i = 0; stop_words[i]; i++) {
        if (strcmp(word, stop_words[i]) == 0) return 1;
    }
    return 0;
}

IgdbGame *igdb_search_game_by_words(const char *title, int platform_id) {
    if (!title || !title[0]) return NULL;

    /* Normalize: lowercase, strip punctuation, collapse spaces */
    char norm[256];
    normalize_title(title, norm, sizeof(norm));
    if (norm[0] == '\0') return NULL;

    /* Tokenize into ALL words (keep originals for sub-word splitting) */
    char all_words[16][64];
    int all_nwords = 0;
    {
        char buf[256];
        strncpy(buf, norm, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        for (char *tok = strtok(buf, " "); tok && all_nwords < 16; tok = strtok(NULL, " ")) {
            strncpy(all_words[all_nwords], tok, 63);
            all_words[all_nwords][63] = '\0';
            all_nwords++;
        }
    }

    /* Filter to words >= 3 chars AND not stop words */
    const char *long_words[8];
    int nlong = 0;
    for (int i = 0; i < all_nwords && nlong < 8; i++) {
        if (strlen(all_words[i]) >= 3 && !is_stop_word(all_words[i]))
            long_words[nlong++] = all_words[i];
    }

    /* --- Phase A: normal word-split (all individual words as substrings) --- */
    if (nlong >= 2) {
        /* Use up to 4 longest words */
        const char *sorted[8];
        int nsorted = nlong < 4 ? nlong : 4;
        memcpy(sorted, long_words, sizeof(const char *) * nlong);
        /* Simple sort by length descending */
        for (int i = 0; i < nsorted - 1; i++) {
            for (int j = i + 1; j < nlong; j++) {
                if (strlen(sorted[j]) > strlen(sorted[i])) {
                    const char *tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }

        printf("IGDB: Word search for \"%s\" (platform %d) [%d words]\n",
               title, platform_id, nsorted);
        IgdbGame *g = igdb_words_query(title, sorted, nsorted, platform_id);
        if (g) return g;
        usleep(260000);

        /* Phase A.5: If 3+ words failed, try just the 2 longest */
        if (nsorted > 2) {
            printf("IGDB: Fallback 2-word search for \"%s\" [%s + %s]\n",
                   title, sorted[0], sorted[1]);
            g = igdb_words_query(title, sorted, 2, platform_id);
            if (g) return g;
            usleep(260000);
        }
    }

    /* --- Phase B: sub-word split for compound words --- */
    /* Find the longest word >= 7 chars (likely a compound like "megaman") */
    int longest_idx = -1;
    size_t longest_len = 0;
    for (int i = 0; i < all_nwords; i++) {
        size_t wlen = strlen(all_words[i]);
        if (wlen >= 7 && wlen > longest_len) {
            longest_len = wlen;
            longest_idx = i;
        }
    }
    if (longest_idx < 0) return NULL;

    /* Try splitting at positions around the middle.
     * "megaman" (7) → try pos 4 ("mega"+"man"), 3 ("meg"+"aman")
     * "ducktales" (9) → try pos 5, 4, 6
     * Each split produces a new query with the sub-words replacing the
     * compound word.  Only 2-3 extra API calls per ROM. */
    int mid = (int)longest_len / 2;
    int splits_to_try[] = { mid, mid + 1, mid - 1 };

    for (int s = 0; s < 3; s++) {
        int sp = splits_to_try[s];
        if (sp < 3 || sp > (int)longest_len - 3) continue;

        /* Build term list: sub-word halves + other words >= 2 chars */
        char part1[64], part2[64];
        strncpy(part1, all_words[longest_idx], sp);
        part1[sp] = '\0';
        strcpy(part2, all_words[longest_idx] + sp);

        const char *terms[12];
        int nterms = 0;
        terms[nterms++] = part1;
        terms[nterms++] = part2;
        for (int i = 0; i < all_nwords && nterms < 12; i++) {
            if (i == longest_idx) continue;
            if (strlen(all_words[i]) >= 2)
                terms[nterms++] = all_words[i];
        }

        printf("IGDB: Compound split \"%s\" → \"%s\"+\"%s\" (platform %d)\n",
               all_words[longest_idx], part1, part2, platform_id);

        IgdbGame *g = igdb_words_query(title, terms, nterms, platform_id);
        if (g) return g;
        usleep(260000);
    }

    return NULL;
}

IgdbGame *igdb_search_by_alt_name(const char *title, int platform_id) {
    if (!title || !title[0]) return NULL;

    /* Escape double quotes */
    char escaped[256];
    {
        const char *s = title;
        char *d = escaped;
        char *end = escaped + sizeof(escaped) - 1;
        while (*s && d < end) {
            if (*s != '"') *d++ = *s;
            s++;
        }
        *d = '\0';
    }

    /* Step 1: Search alternative_names for matching entries */
    char body[512];
    snprintf(body, sizeof(body),
        "fields game;\n"
        "where name ~ *\"%s\"*;\n"
        "limit 10;",
        escaped);

    char *response = igdb_request("alternative_names", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json || !cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Collect unique game IDs */
    int game_ids[10];
    int ngames = 0;
    int n = cJSON_GetArraySize(json);
    for (int i = 0; i < n && ngames < 10; i++) {
        cJSON *item = cJSON_GetArrayItem(json, i);
        if (!item) continue;
        cJSON *gid = cJSON_GetObjectItem(item, "game");
        if (!gid || !cJSON_IsNumber(gid)) continue;
        int id = gid->valueint;
        /* Deduplicate */
        int dup = 0;
        for (int j = 0; j < ngames; j++) {
            if (game_ids[j] == id) { dup = 1; break; }
        }
        if (!dup) game_ids[ngames++] = id;
    }
    cJSON_Delete(json);

    if (ngames == 0) return NULL;

    /* Step 2: Fetch game details for matched IDs with platform filter */
    char ids_str[256];
    int pos = 0;
    for (int i = 0; i < ngames; i++) {
        if (i > 0) pos += snprintf(ids_str + pos, sizeof(ids_str) - pos, ",");
        pos += snprintf(ids_str + pos, sizeof(ids_str) - pos, "%d", game_ids[i]);
    }

    char body2[1024];
    pos = snprintf(body2, sizeof(body2),
        "fields name,summary,first_release_date,genres.name,platforms.name,"
        "involved_companies.company.name,involved_companies.developer,"
        "involved_companies.publisher,total_rating,cover.image_id;\n"
        "where id = (%s)", ids_str);
    if (platform_id > 0)
        pos += snprintf(body2 + pos, sizeof(body2) - pos,
            " & platforms = (%d)", platform_id);
    snprintf(body2 + pos, sizeof(body2) - pos, ";\nlimit 10;");

    usleep(260000);
    response = igdb_request("games", body2);
    if (!response) return NULL;

    json = cJSON_Parse(response);
    free(response);
    if (!json || !cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    /* Score results against the search title */
    int best_score = 0;
    cJSON *best_game = NULL;
    n = cJSON_GetArraySize(json);
    for (int i = 0; i < n; i++) {
        cJSON *game = cJSON_GetArrayItem(json, i);
        if (!game) continue;
        cJSON *name_item = cJSON_GetObjectItem(game, "name");
        if (!name_item || !cJSON_IsString(name_item)) continue;
        int score = title_match_score(title, name_item->valuestring);
        /* For alt-name matches, accept lower threshold since the name
         * may be quite different from the search title */
        if (score > best_score) { best_score = score; best_game = game; }
    }

    /* Accept any match found via alternative_names (threshold 20) since
     * we know the alt name matched the search string */
    if (best_score < 20 || !best_game) {
        /* If scoring against game name fails, just take the first result
         * since we found it via alternative_names match */
        best_game = cJSON_GetArrayItem(json, 0);
        if (!best_game) {
            cJSON_Delete(json);
            return NULL;
        }
    }

    if (igdb_logfile) {
        cJSON *bn = cJSON_GetObjectItem(best_game, "name");
        fprintf(igdb_logfile, "    -> alt_name: %d results, matched: \"%s\" score=%d\n",
            n, (bn && cJSON_IsString(bn)) ? bn->valuestring : "?", best_score);
    }

    IgdbGame *g = parse_igdb_game(best_game);
    cJSON_Delete(json);
    return g;
}

char *igdb_get_game_cover(int igdb_id) {
    if (igdb_id <= 0) return NULL;

    char body[256];
    snprintf(body, sizeof(body),
        "where id = %d;\n"
        "fields cover.image_id;\n"
        "limit 1;",
        igdb_id);

    char *response = igdb_request("games", body);
    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json || !cJSON_IsArray(json) || cJSON_GetArraySize(json) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    cJSON *game = cJSON_GetArrayItem(json, 0);
    char *cover_url = NULL;

    if (game) {
        cJSON *cover = cJSON_GetObjectItem(game, "cover");
        if (cover) {
            if (cJSON_IsObject(cover)) {
                cJSON *image_id = cJSON_GetObjectItem(cover, "image_id");
                if (image_id && cJSON_IsString(image_id)) {
                    char url[512];
                    snprintf(url, sizeof(url),
                        "https://images.igdb.com/igdb/image/upload/t_cover_big/%s.jpg",
                        image_id->valuestring);
                    cover_url = strdup(url);
                }
            } else if (cJSON_IsNumber(cover)) {
                cover_url = igdb_fetch_cover_url(cover->valueint);
            }
        }
    }

    cJSON_Delete(json);
    return cover_url;
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
