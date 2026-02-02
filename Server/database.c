/*
 * Nixly Media Server - Database Module
 * SQLite-based media library database with TMDB metadata
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "database.h"

static sqlite3 *db = NULL;

/* Helper to escape strings for JSON */
static char *json_escape(const char *str) {
    if (!str) return strdup("null");

    size_t len = strlen(str);
    char *escaped = malloc(len * 2 + 3);
    char *p = escaped;

    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '"':  *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\n': *p++ = '\\'; *p++ = 'n'; break;
            case '\r': *p++ = '\\'; *p++ = 'r'; break;
            case '\t': *p++ = '\\'; *p++ = 't'; break;
            default:   *p++ = str[i]; break;
        }
    }
    *p++ = '"';
    *p = '\0';

    return escaped;
}

int database_init(const char *db_path) {
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    /* Enable WAL mode for better concurrent access */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    /* Create media table with TMDB fields */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS media ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  type INTEGER NOT NULL,"
        "  title TEXT NOT NULL,"
        "  filepath TEXT UNIQUE NOT NULL,"
        "  show_name TEXT,"
        "  season INTEGER,"
        "  episode INTEGER,"
        "  size INTEGER,"
        "  duration INTEGER,"
        "  width INTEGER,"
        "  height INTEGER,"
        "  codec_video TEXT,"
        "  codec_audio TEXT,"
        "  bitrate INTEGER,"
        "  added_date TEXT DEFAULT (datetime('now')),"
        /* TMDB fields */
        "  tmdb_id INTEGER,"
        "  tmdb_title TEXT,"
        "  overview TEXT,"
        "  poster_path TEXT,"
        "  backdrop_path TEXT,"
        "  release_date TEXT,"
        "  year INTEGER,"
        "  rating REAL,"
        "  vote_count INTEGER,"
        "  genres TEXT,"
        "  tmdb_show_id INTEGER,"
        "  episode_title TEXT,"
        "  episode_overview TEXT,"
        "  still_path TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_type ON media(type);"
        "CREATE INDEX IF NOT EXISTS idx_show ON media(show_name);"
        "CREATE INDEX IF NOT EXISTS idx_filepath ON media(filepath);"
        "CREATE INDEX IF NOT EXISTS idx_tmdb ON media(tmdb_id);"
        /* Retro gaming ROMs table */
        "CREATE TABLE IF NOT EXISTS roms ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  console INTEGER NOT NULL,"
        "  title TEXT NOT NULL,"
        "  filepath TEXT UNIQUE NOT NULL,"
        "  cover_path TEXT,"
        "  size INTEGER,"
        "  region TEXT,"
        "  added_date TEXT DEFAULT (datetime('now'))"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_console ON roms(console);"
        "CREATE INDEX IF NOT EXISTS idx_rom_filepath ON roms(filepath);";

    char *err_msg = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* Add TMDB columns if they don't exist (for upgrades) */
    const char *alter_sqls[] = {
        "ALTER TABLE media ADD COLUMN tmdb_id INTEGER",
        "ALTER TABLE media ADD COLUMN tmdb_title TEXT",
        "ALTER TABLE media ADD COLUMN overview TEXT",
        "ALTER TABLE media ADD COLUMN poster_path TEXT",
        "ALTER TABLE media ADD COLUMN backdrop_path TEXT",
        "ALTER TABLE media ADD COLUMN release_date TEXT",
        "ALTER TABLE media ADD COLUMN year INTEGER",
        "ALTER TABLE media ADD COLUMN rating REAL",
        "ALTER TABLE media ADD COLUMN vote_count INTEGER",
        "ALTER TABLE media ADD COLUMN genres TEXT",
        "ALTER TABLE media ADD COLUMN tmdb_show_id INTEGER",
        "ALTER TABLE media ADD COLUMN episode_title TEXT",
        "ALTER TABLE media ADD COLUMN episode_overview TEXT",
        "ALTER TABLE media ADD COLUMN still_path TEXT",
        NULL
    };

    for (int i = 0; alter_sqls[i]; i++) {
        sqlite3_exec(db, alter_sqls[i], NULL, NULL, NULL); /* Ignore errors (column exists) */
    }

    return 0;
}

void database_close(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

int database_add_media(MediaEntry *entry) {
    const char *sql =
        "INSERT OR REPLACE INTO media "
        "(type, title, filepath, show_name, season, episode, size, duration, "
        "width, height, codec_video, codec_audio, bitrate) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, entry->type);
    sqlite3_bind_text(stmt, 2, entry->title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, entry->filepath, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, entry->show_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, entry->season);
    sqlite3_bind_int(stmt, 6, entry->episode);
    sqlite3_bind_int64(stmt, 7, entry->size);
    sqlite3_bind_int(stmt, 8, entry->duration);
    sqlite3_bind_int(stmt, 9, entry->width);
    sqlite3_bind_int(stmt, 10, entry->height);
    sqlite3_bind_text(stmt, 11, entry->codec_video, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 12, entry->codec_audio, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 13, entry->bitrate);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Insert failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return (int)sqlite3_last_insert_rowid(db);
}

int database_update_tmdb(int id, MediaEntry *tmdb_data) {
    const char *sql =
        "UPDATE media SET "
        "tmdb_id=?, tmdb_title=?, overview=?, poster_path=?, backdrop_path=?, "
        "release_date=?, year=?, rating=?, vote_count=?, genres=?, "
        "tmdb_show_id=?, episode_title=?, episode_overview=?, still_path=? "
        "WHERE id=?";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, tmdb_data->tmdb_id);
    sqlite3_bind_text(stmt, 2, tmdb_data->tmdb_title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, tmdb_data->overview, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, tmdb_data->poster_path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, tmdb_data->backdrop_path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, tmdb_data->release_date, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 7, tmdb_data->year);
    sqlite3_bind_double(stmt, 8, tmdb_data->rating);
    sqlite3_bind_int(stmt, 9, tmdb_data->vote_count);
    sqlite3_bind_text(stmt, 10, tmdb_data->genres, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 11, tmdb_data->tmdb_show_id);
    sqlite3_bind_text(stmt, 12, tmdb_data->episode_title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, tmdb_data->episode_overview, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 14, tmdb_data->still_path, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 15, id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int database_file_exists(const char *filepath) {
    const char *sql = "SELECT 1 FROM media WHERE filepath = ? LIMIT 1";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return exists;
}

int database_get_id_by_path(const char *filepath) {
    const char *sql = "SELECT id FROM media WHERE filepath = ?";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);
    int id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    return id;
}

int database_delete_by_path(const char *filepath) {
    const char *sql = "DELETE FROM media WHERE filepath = ?";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int database_cleanup_missing(void) {
    const char *sql = "SELECT id, filepath FROM media";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    /* Collect IDs of missing files */
    int *missing_ids = NULL;
    int missing_count = 0;
    int missing_capacity = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *filepath = (const char *)sqlite3_column_text(stmt, 1);

        /* Check if file exists */
        struct stat st;
        if (stat(filepath, &st) != 0) {
            /* File doesn't exist, mark for deletion */
            if (missing_count >= missing_capacity) {
                missing_capacity = missing_capacity ? missing_capacity * 2 : 64;
                missing_ids = realloc(missing_ids, missing_capacity * sizeof(int));
            }
            missing_ids[missing_count++] = id;
            printf("  Removing missing: %s\n", filepath);
        }
    }
    sqlite3_finalize(stmt);

    /* Delete missing entries */
    if (missing_count > 0) {
        const char *delete_sql = "DELETE FROM media WHERE id = ?";
        sqlite3_stmt *delete_stmt;

        if (sqlite3_prepare_v2(db, delete_sql, -1, &delete_stmt, NULL) == SQLITE_OK) {
            for (int i = 0; i < missing_count; i++) {
                sqlite3_bind_int(delete_stmt, 1, missing_ids[i]);
                sqlite3_step(delete_stmt);
                sqlite3_reset(delete_stmt);
            }
            sqlite3_finalize(delete_stmt);
        }
    }

    free(missing_ids);
    return missing_count;
}

int database_get_count(void) {
    const char *sql = "SELECT COUNT(*) FROM media";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    return count;
}

char *database_get_filepath(int id) {
    const char *sql = "SELECT filepath FROM media WHERE id = ?";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, id);

    char *filepath = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        if (path) {
            filepath = strdup(path);
        }
    }
    sqlite3_finalize(stmt);

    return filepath;
}

int database_get_entries_without_tmdb(MediaEntry **entries, int *count) {
    const char *sql = "SELECT id, type, title, show_name, season, episode, filepath "
                      "FROM media WHERE tmdb_id IS NULL OR tmdb_id = 0";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    /* Count first */
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) n++;
    sqlite3_reset(stmt);

    if (n == 0) {
        *entries = NULL;
        *count = 0;
        sqlite3_finalize(stmt);
        return 0;
    }

    *entries = calloc(n, sizeof(MediaEntry));
    *count = n;

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < n) {
        (*entries)[i].id = sqlite3_column_int(stmt, 0);
        (*entries)[i].type = sqlite3_column_int(stmt, 1);
        const char *t = (const char *)sqlite3_column_text(stmt, 2);
        (*entries)[i].title = t ? strdup(t) : NULL;
        const char *s = (const char *)sqlite3_column_text(stmt, 3);
        (*entries)[i].show_name = s ? strdup(s) : NULL;
        (*entries)[i].season = sqlite3_column_int(stmt, 4);
        (*entries)[i].episode = sqlite3_column_int(stmt, 5);
        const char *f = (const char *)sqlite3_column_text(stmt, 6);
        (*entries)[i].filepath = f ? strdup(f) : NULL;
        i++;
    }

    sqlite3_finalize(stmt);
    return 0;
}

/* Build JSON with TMDB data */
static char *build_json_array(const char *sql) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    size_t buf_size = 65536;
    size_t buf_used = 0;
    char *json = malloc(buf_size);
    json[buf_used++] = '[';

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (buf_used > buf_size - 8192) {
            buf_size *= 2;
            json = realloc(json, buf_size);
        }

        if (!first) json[buf_used++] = ',';
        first = 0;

        int id = sqlite3_column_int(stmt, 0);
        int type = sqlite3_column_int(stmt, 1);
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *show_name = (const char *)sqlite3_column_text(stmt, 3);
        int season = sqlite3_column_int(stmt, 4);
        int episode = sqlite3_column_int(stmt, 5);
        int64_t size = sqlite3_column_int64(stmt, 6);
        int duration = sqlite3_column_int(stmt, 7);
        int width = sqlite3_column_int(stmt, 8);
        int height = sqlite3_column_int(stmt, 9);
        /* TMDB fields */
        int tmdb_id = sqlite3_column_int(stmt, 10);
        const char *tmdb_title = (const char *)sqlite3_column_text(stmt, 11);
        const char *overview = (const char *)sqlite3_column_text(stmt, 12);
        const char *poster = (const char *)sqlite3_column_text(stmt, 13);
        const char *backdrop = (const char *)sqlite3_column_text(stmt, 14);
        int year = sqlite3_column_int(stmt, 15);
        float rating = (float)sqlite3_column_double(stmt, 16);
        const char *genres = (const char *)sqlite3_column_text(stmt, 17);

        char *title_esc = json_escape(title);
        char *show_esc = json_escape(show_name);
        char *tmdb_title_esc = json_escape(tmdb_title);
        char *overview_esc = json_escape(overview);
        char *poster_esc = json_escape(poster);
        char *backdrop_esc = json_escape(backdrop);
        char *genres_esc = json_escape(genres);

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"id\":%d,\"type\":%d,\"title\":%s,\"show_name\":%s,"
            "\"season\":%d,\"episode\":%d,\"size\":%ld,\"duration\":%d,"
            "\"width\":%d,\"height\":%d,"
            "\"tmdb_id\":%d,\"tmdb_title\":%s,\"overview\":%s,"
            "\"poster\":%s,\"backdrop\":%s,\"year\":%d,\"rating\":%.1f,"
            "\"genres\":%s}",
            id, type, title_esc, show_esc,
            season, episode, size, duration, width, height,
            tmdb_id, tmdb_title_esc, overview_esc,
            poster_esc, backdrop_esc, year, rating, genres_esc);

        free(title_esc);
        free(show_esc);
        free(tmdb_title_esc);
        free(overview_esc);
        free(poster_esc);
        free(backdrop_esc);
        free(genres_esc);
    }

    json[buf_used++] = ']';
    json[buf_used] = '\0';

    sqlite3_finalize(stmt);
    return json;
}

char *database_get_library_json(void) {
    return build_json_array(
        "SELECT id, type, title, show_name, season, episode, size, duration, "
        "width, height, tmdb_id, tmdb_title, overview, poster_path, backdrop_path, "
        "year, rating, genres FROM media ORDER BY type, show_name, season, episode, title"
    );
}

char *database_get_movies_json(void) {
    return build_json_array(
        "SELECT id, type, title, show_name, season, episode, size, duration, "
        "width, height, tmdb_id, tmdb_title, overview, poster_path, backdrop_path, "
        "year, rating, genres FROM media WHERE type = 0 ORDER BY COALESCE(tmdb_title, title)"
    );
}

char *database_get_tvshows_json(void) {
    /* Group by show - return one entry per unique show with episode count */
    /* Use MAX() for poster/backdrop to get first non-NULL value */
    const char *sql =
        "SELECT MIN(id), MAX(type), MAX(title), MAX(show_name), MIN(season), COUNT(*) as episode_count, "
        "SUM(size), SUM(duration), MAX(width), MAX(height), "
        "MAX(tmdb_id), MAX(tmdb_title), MAX(overview), MAX(poster_path), MAX(backdrop_path), "
        "MAX(year), MAX(rating), MAX(genres) "
        "FROM media WHERE type IN (1, 2) "
        "GROUP BY COALESCE(tmdb_title, show_name, title) "
        "ORDER BY COALESCE(tmdb_title, show_name, title)";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    size_t buf_size = 65536;
    size_t buf_used = 0;
    char *json = malloc(buf_size);
    json[buf_used++] = '[';

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (buf_used > buf_size - 8192) {
            buf_size *= 2;
            json = realloc(json, buf_size);
        }

        if (!first) json[buf_used++] = ',';
        first = 0;

        int id = sqlite3_column_int(stmt, 0);
        int type = sqlite3_column_int(stmt, 1);
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *show_name = (const char *)sqlite3_column_text(stmt, 3);
        int season = sqlite3_column_int(stmt, 4);
        int episode_count = sqlite3_column_int(stmt, 5);
        int64_t size = sqlite3_column_int64(stmt, 6);
        int duration = sqlite3_column_int(stmt, 7);
        int width = sqlite3_column_int(stmt, 8);
        int height = sqlite3_column_int(stmt, 9);
        int tmdb_id = sqlite3_column_int(stmt, 10);
        const char *tmdb_title = (const char *)sqlite3_column_text(stmt, 11);
        const char *overview = (const char *)sqlite3_column_text(stmt, 12);
        const char *poster = (const char *)sqlite3_column_text(stmt, 13);
        const char *backdrop = (const char *)sqlite3_column_text(stmt, 14);
        int year = sqlite3_column_int(stmt, 15);
        float rating = (float)sqlite3_column_double(stmt, 16);
        const char *genres = (const char *)sqlite3_column_text(stmt, 17);

        char *title_esc = json_escape(title);
        char *show_esc = json_escape(show_name);
        char *tmdb_title_esc = json_escape(tmdb_title);
        char *overview_esc = json_escape(overview);
        char *poster_esc = json_escape(poster);
        char *backdrop_esc = json_escape(backdrop);
        char *genres_esc = json_escape(genres);

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"id\":%d,\"type\":%d,\"title\":%s,\"show_name\":%s,"
            "\"season\":%d,\"episode_count\":%d,\"size\":%ld,\"duration\":%d,"
            "\"width\":%d,\"height\":%d,"
            "\"tmdb_id\":%d,\"tmdb_title\":%s,\"overview\":%s,"
            "\"poster\":%s,\"backdrop\":%s,\"year\":%d,\"rating\":%.1f,"
            "\"genres\":%s}",
            id, type, title_esc, show_esc,
            season, episode_count, size, duration, width, height,
            tmdb_id, tmdb_title_esc, overview_esc,
            poster_esc, backdrop_esc, year, rating, genres_esc);

        free(title_esc);
        free(show_esc);
        free(tmdb_title_esc);
        free(overview_esc);
        free(poster_esc);
        free(backdrop_esc);
        free(genres_esc);
    }

    json[buf_used++] = ']';
    json[buf_used] = '\0';

    sqlite3_finalize(stmt);
    return json;
}

char *database_get_media_json(int id) {
    const char *sql =
        "SELECT id, type, title, filepath, show_name, season, episode, size, "
        "duration, width, height, codec_video, codec_audio, bitrate, added_date, "
        "tmdb_id, tmdb_title, overview, poster_path, backdrop_path, release_date, "
        "year, rating, vote_count, genres, episode_title, episode_overview, still_path "
        "FROM media WHERE id = ?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, id);

    char *json = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *filepath = (const char *)sqlite3_column_text(stmt, 3);
        const char *show_name = (const char *)sqlite3_column_text(stmt, 4);
        const char *codec_v = (const char *)sqlite3_column_text(stmt, 11);
        const char *codec_a = (const char *)sqlite3_column_text(stmt, 12);
        const char *added = (const char *)sqlite3_column_text(stmt, 14);
        const char *tmdb_title = (const char *)sqlite3_column_text(stmt, 16);
        const char *overview = (const char *)sqlite3_column_text(stmt, 17);
        const char *poster = (const char *)sqlite3_column_text(stmt, 18);
        const char *backdrop = (const char *)sqlite3_column_text(stmt, 19);
        const char *release = (const char *)sqlite3_column_text(stmt, 20);
        const char *genres = (const char *)sqlite3_column_text(stmt, 24);
        const char *ep_title = (const char *)sqlite3_column_text(stmt, 25);
        const char *ep_overview = (const char *)sqlite3_column_text(stmt, 26);
        const char *still = (const char *)sqlite3_column_text(stmt, 27);

        char *title_esc = json_escape(title);
        char *path_esc = json_escape(filepath);
        char *show_esc = json_escape(show_name);
        char *cv_esc = json_escape(codec_v);
        char *ca_esc = json_escape(codec_a);
        char *added_esc = json_escape(added);
        char *tmdb_esc = json_escape(tmdb_title);
        char *ov_esc = json_escape(overview);
        char *poster_esc = json_escape(poster);
        char *back_esc = json_escape(backdrop);
        char *rel_esc = json_escape(release);
        char *genres_esc = json_escape(genres);
        char *ept_esc = json_escape(ep_title);
        char *epo_esc = json_escape(ep_overview);
        char *still_esc = json_escape(still);

        json = malloc(16384);
        snprintf(json, 16384,
            "{\"id\":%d,\"type\":%d,\"title\":%s,\"filepath\":%s,"
            "\"show_name\":%s,\"season\":%d,\"episode\":%d,"
            "\"size\":%ld,\"duration\":%d,\"width\":%d,\"height\":%d,"
            "\"codec_video\":%s,\"codec_audio\":%s,\"bitrate\":%ld,"
            "\"added_date\":%s,\"stream_url\":\"/stream/%d\","
            "\"tmdb_id\":%d,\"tmdb_title\":%s,\"overview\":%s,"
            "\"poster\":%s,\"backdrop\":%s,\"release_date\":%s,"
            "\"year\":%d,\"rating\":%.1f,\"vote_count\":%d,\"genres\":%s,"
            "\"episode_title\":%s,\"episode_overview\":%s,\"still\":%s}",
            sqlite3_column_int(stmt, 0),
            sqlite3_column_int(stmt, 1),
            title_esc, path_esc, show_esc,
            sqlite3_column_int(stmt, 5),
            sqlite3_column_int(stmt, 6),
            sqlite3_column_int64(stmt, 7),
            sqlite3_column_int(stmt, 8),
            sqlite3_column_int(stmt, 9),
            sqlite3_column_int(stmt, 10),
            cv_esc, ca_esc,
            sqlite3_column_int64(stmt, 13),
            added_esc,
            sqlite3_column_int(stmt, 0),
            sqlite3_column_int(stmt, 15),
            tmdb_esc, ov_esc, poster_esc, back_esc, rel_esc,
            sqlite3_column_int(stmt, 21),
            sqlite3_column_double(stmt, 22),
            sqlite3_column_int(stmt, 23),
            genres_esc, ept_esc, epo_esc, still_esc);

        free(title_esc); free(path_esc); free(show_esc);
        free(cv_esc); free(ca_esc); free(added_esc);
        free(tmdb_esc); free(ov_esc); free(poster_esc);
        free(back_esc); free(rel_esc); free(genres_esc);
        free(ept_esc); free(epo_esc); free(still_esc);
    }

    sqlite3_finalize(stmt);
    return json;
}

void database_free_entry(MediaEntry *entry) {
    if (entry) {
        free(entry->title);
        free(entry->filepath);
        free(entry->show_name);
        free(entry->codec_video);
        free(entry->codec_audio);
        free(entry->added_date);
        free(entry->tmdb_title);
        free(entry->overview);
        free(entry->poster_path);
        free(entry->backdrop_path);
        free(entry->release_date);
        free(entry->genres);
        free(entry->episode_title);
        free(entry->episode_overview);
        free(entry->still_path);
    }
}

/* Console name helper */
static const char *console_names[] = {
    "NES", "SNES", "Nintendo 64", "GameCube", "Wii", "Switch"
};

const char *database_console_name(ConsoleType console) {
    if (console >= 0 && console < CONSOLE_COUNT) {
        return console_names[console];
    }
    return "Unknown";
}

/* Retro gaming ROM functions */
int database_add_rom(RomEntry *entry) {
    const char *sql =
        "INSERT OR REPLACE INTO roms "
        "(console, title, filepath, cover_path, size, region) "
        "VALUES (?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, entry->console);
    sqlite3_bind_text(stmt, 2, entry->title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, entry->filepath, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, entry->cover_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, entry->size);
    sqlite3_bind_text(stmt, 6, entry->region, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Insert ROM failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return 0;
}

int database_rom_exists(const char *filepath) {
    const char *sql = "SELECT 1 FROM roms WHERE filepath = ? LIMIT 1";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return exists;
}

int database_get_rom_count(void) {
    const char *sql = "SELECT COUNT(*) FROM roms";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    return count;
}

int database_get_rom_count_by_console(ConsoleType console) {
    const char *sql = "SELECT COUNT(*) FROM roms WHERE console = ?";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int(stmt, 1, console);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    return count;
}

char *database_get_rom_filepath(int id) {
    const char *sql = "SELECT filepath FROM roms WHERE id = ?";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, id);

    char *filepath = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        if (path) {
            filepath = strdup(path);
        }
    }
    sqlite3_finalize(stmt);

    return filepath;
}

char *database_get_roms_json(void) {
    const char *sql =
        "SELECT id, console, title, cover_path, size, region "
        "FROM roms ORDER BY console, title";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    size_t buf_size = 65536;
    size_t buf_used = 0;
    char *json = malloc(buf_size);
    json[buf_used++] = '[';

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (buf_used > buf_size - 2048) {
            buf_size *= 2;
            json = realloc(json, buf_size);
        }

        if (!first) json[buf_used++] = ',';
        first = 0;

        int id = sqlite3_column_int(stmt, 0);
        int console = sqlite3_column_int(stmt, 1);
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *cover = (const char *)sqlite3_column_text(stmt, 3);
        int64_t size = sqlite3_column_int64(stmt, 4);
        const char *region = (const char *)sqlite3_column_text(stmt, 5);

        char *title_esc = json_escape(title);
        char *cover_esc = json_escape(cover);
        char *region_esc = json_escape(region);

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"id\":%d,\"console\":%d,\"console_name\":\"%s\","
            "\"title\":%s,\"cover\":%s,\"size\":%ld,\"region\":%s}",
            id, console, database_console_name(console),
            title_esc, cover_esc, size, region_esc);

        free(title_esc);
        free(cover_esc);
        free(region_esc);
    }

    json[buf_used++] = ']';
    json[buf_used] = '\0';

    sqlite3_finalize(stmt);
    return json;
}

char *database_get_roms_by_console_json(ConsoleType console) {
    const char *sql =
        "SELECT id, console, title, cover_path, size, region "
        "FROM roms WHERE console = ? ORDER BY title";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, console);

    size_t buf_size = 65536;
    size_t buf_used = 0;
    char *json = malloc(buf_size);
    json[buf_used++] = '[';

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (buf_used > buf_size - 2048) {
            buf_size *= 2;
            json = realloc(json, buf_size);
        }

        if (!first) json[buf_used++] = ',';
        first = 0;

        int id = sqlite3_column_int(stmt, 0);
        int cons = sqlite3_column_int(stmt, 1);
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *cover = (const char *)sqlite3_column_text(stmt, 3);
        int64_t size = sqlite3_column_int64(stmt, 4);
        const char *region = (const char *)sqlite3_column_text(stmt, 5);

        char *title_esc = json_escape(title);
        char *cover_esc = json_escape(cover);
        char *region_esc = json_escape(region);

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"id\":%d,\"console\":%d,\"console_name\":\"%s\","
            "\"title\":%s,\"cover\":%s,\"size\":%ld,\"region\":%s}",
            id, cons, database_console_name(cons),
            title_esc, cover_esc, size, region_esc);

        free(title_esc);
        free(cover_esc);
        free(region_esc);
    }

    json[buf_used++] = ']';
    json[buf_used] = '\0';

    sqlite3_finalize(stmt);
    return json;
}

char *database_get_rom_json(int id) {
    const char *sql =
        "SELECT id, console, title, filepath, cover_path, size, region, added_date "
        "FROM roms WHERE id = ?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, id);

    char *json = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int cons = sqlite3_column_int(stmt, 1);
        const char *title = (const char *)sqlite3_column_text(stmt, 2);
        const char *filepath = (const char *)sqlite3_column_text(stmt, 3);
        const char *cover = (const char *)sqlite3_column_text(stmt, 4);
        const char *region = (const char *)sqlite3_column_text(stmt, 6);
        const char *added = (const char *)sqlite3_column_text(stmt, 7);

        char *title_esc = json_escape(title);
        char *path_esc = json_escape(filepath);
        char *cover_esc = json_escape(cover);
        char *region_esc = json_escape(region);
        char *added_esc = json_escape(added);

        json = malloc(4096);
        snprintf(json, 4096,
            "{\"id\":%d,\"console\":%d,\"console_name\":\"%s\","
            "\"title\":%s,\"filepath\":%s,\"cover\":%s,"
            "\"size\":%ld,\"region\":%s,\"added_date\":%s}",
            sqlite3_column_int(stmt, 0), cons, database_console_name(cons),
            title_esc, path_esc, cover_esc,
            sqlite3_column_int64(stmt, 5), region_esc, added_esc);

        free(title_esc);
        free(path_esc);
        free(cover_esc);
        free(region_esc);
        free(added_esc);
    }

    sqlite3_finalize(stmt);
    return json;
}

void database_free_rom(RomEntry *entry) {
    if (entry) {
        free(entry->title);
        free(entry->filepath);
        free(entry->cover_path);
        free(entry->region);
        free(entry->added_date);
    }
}
