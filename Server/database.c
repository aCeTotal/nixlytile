/*
 * Nixly Media Server - Database Module
 * SQLite-based media library database
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* Create tables */
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
        "  added_date TEXT DEFAULT (datetime('now'))"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_type ON media(type);"
        "CREATE INDEX IF NOT EXISTS idx_show ON media(show_name);"
        "CREATE INDEX IF NOT EXISTS idx_filepath ON media(filepath);";

    char *err_msg = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
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

    return 0;
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

/* Build JSON array from query results */
static char *build_json_array(const char *sql) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    /* Allocate buffer for JSON */
    size_t buf_size = 65536;
    size_t buf_used = 0;
    char *json = malloc(buf_size);
    json[buf_used++] = '[';

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        /* Ensure buffer space */
        if (buf_used > buf_size - 4096) {
            buf_size *= 2;
            json = realloc(json, buf_size);
        }

        if (!first) {
            json[buf_used++] = ',';
        }
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

        char *title_esc = json_escape(title);
        char *show_esc = json_escape(show_name);

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"id\":%d,\"type\":%d,\"title\":%s,\"show_name\":%s,"
            "\"season\":%d,\"episode\":%d,\"size\":%ld,\"duration\":%d,"
            "\"width\":%d,\"height\":%d}",
            id, type, title_esc, show_esc,
            season, episode, size, duration, width, height);

        free(title_esc);
        free(show_esc);
    }

    json[buf_used++] = ']';
    json[buf_used] = '\0';

    sqlite3_finalize(stmt);
    return json;
}

char *database_get_library_json(void) {
    return build_json_array(
        "SELECT id, type, title, show_name, season, episode, size, "
        "duration, width, height FROM media ORDER BY type, show_name, season, episode, title"
    );
}

char *database_get_movies_json(void) {
    return build_json_array(
        "SELECT id, type, title, show_name, season, episode, size, "
        "duration, width, height FROM media WHERE type = 0 ORDER BY title"
    );
}

char *database_get_tvshows_json(void) {
    return build_json_array(
        "SELECT id, type, title, show_name, season, episode, size, "
        "duration, width, height FROM media WHERE type IN (1, 2) "
        "ORDER BY show_name, season, episode"
    );
}

char *database_get_media_json(int id) {
    const char *sql =
        "SELECT id, type, title, filepath, show_name, season, episode, size, "
        "duration, width, height, codec_video, codec_audio, bitrate, added_date "
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

        char *title_esc = json_escape(title);
        char *path_esc = json_escape(filepath);
        char *show_esc = json_escape(show_name);
        char *cv_esc = json_escape(codec_v);
        char *ca_esc = json_escape(codec_a);
        char *added_esc = json_escape(added);

        json = malloc(4096);
        snprintf(json, 4096,
            "{\"id\":%d,\"type\":%d,\"title\":%s,\"filepath\":%s,"
            "\"show_name\":%s,\"season\":%d,\"episode\":%d,"
            "\"size\":%ld,\"duration\":%d,\"width\":%d,\"height\":%d,"
            "\"codec_video\":%s,\"codec_audio\":%s,\"bitrate\":%ld,"
            "\"added_date\":%s,\"stream_url\":\"/stream/%d\"}",
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
            sqlite3_column_int(stmt, 0));

        free(title_esc);
        free(path_esc);
        free(show_esc);
        free(cv_esc);
        free(ca_esc);
        free(added_esc);
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
    }
}
