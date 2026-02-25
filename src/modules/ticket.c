#include "ticket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>

// STB Image libraries for image processing
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

// Module-private state
static Database *g_db = NULL;
static struct discord *g_client = NULL;

// Directory for storing ticket media
#define TICKET_IMAGES_DIR "ticket_images"

// Attachment media types stored in the DB
typedef enum {
    MEDIA_TYPE_OTHER = 0,
    MEDIA_TYPE_IMAGE = 1,
    MEDIA_TYPE_VIDEO = 2,
} MediaType;

// SQL schema for ticket tables
static const char *CREATE_TICKET_TABLES_SQL =
    "CREATE TABLE IF NOT EXISTS server_configs ("
    "    main_guild_id INTEGER PRIMARY KEY,"
    "    staff_guild_id INTEGER NOT NULL,"
    "    ticket_category_id INTEGER NOT NULL,"
    "    log_channel_id INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS tickets ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    user_id INTEGER NOT NULL,"
    "    main_guild_id INTEGER NOT NULL,"
    "    staff_guild_id INTEGER NOT NULL,"
    "    staff_channel_id INTEGER NOT NULL,"
    "    dm_channel_id INTEGER NOT NULL,"
    "    status INTEGER DEFAULT 0,"
    "    created_at INTEGER DEFAULT (strftime('%s', 'now')),"
    "    closed_at INTEGER"
    ");"

    "CREATE TABLE IF NOT EXISTS ticket_messages ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    ticket_id INTEGER NOT NULL,"
    "    message_id INTEGER NOT NULL,"
    "    author_id INTEGER NOT NULL,"
    "    content TEXT NOT NULL,"
    "    attachments_json TEXT,"
    "    from_user INTEGER NOT NULL,"
    "    is_deleted INTEGER DEFAULT 0,"
    "    is_edited INTEGER DEFAULT 0,"
    "    timestamp INTEGER DEFAULT (strftime('%s', 'now')),"
    "    edited_at INTEGER,"
    "    FOREIGN KEY (ticket_id) REFERENCES tickets(id)"
    ");"

    "CREATE TABLE IF NOT EXISTS ticket_attachments ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    message_id INTEGER NOT NULL,"
    "    original_url TEXT NOT NULL,"
    "    local_path TEXT,"
    "    filename TEXT NOT NULL,"
    "    media_type INTEGER DEFAULT 0,"   /* 0=other, 1=image, 2=video */
    "    download_status INTEGER DEFAULT 0,"
    "    FOREIGN KEY (message_id) REFERENCES ticket_messages(id)"
    ");"

    "CREATE INDEX IF NOT EXISTS idx_message_id ON ticket_messages(message_id);";

/* -------------------------------------------------------------------------
 * libcurl helpers
 * ---------------------------------------------------------------------- */

struct MemoryStruct {
    unsigned char *memory;
    size_t size;
};

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    unsigned char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "[ticket] realloc failed in write_memory_callback\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

/* Download URL into a heap buffer.  Caller owns memory->memory. */
static int curl_download(const char *url, struct MemoryStruct *out) {
    out->memory = malloc(1);
    out->size   = 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[ticket] curl_easy_init() failed\n");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      (void *)out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "Discord-Ticket-Bot/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[ticket] curl error downloading %.80s: %s\n",
                url, curl_easy_strerror(res));
        free(out->memory);
        out->memory = NULL;
        out->size   = 0;
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "[ticket] HTTP %ld downloading %.80s\n", http_code, url);
        free(out->memory);
        out->memory = NULL;
        out->size   = 0;
        return -1;
    }

    printf("[ticket] Downloaded %zu bytes (HTTP %ld)\n", out->size, http_code);
    return 0;
}

/* -------------------------------------------------------------------------
 * Extension / MIME helpers
 *
 * Discord CDN URLs look like:
 *   https://cdn.discordapp.com/attachments/111/222/photo.jpg?ex=abc&is=def
 * Checking the raw URL tail will fail because of the query string.
 * We must strip the query component and inspect the filename instead.
 * ---------------------------------------------------------------------- */

/* Fills ext_out with the lower-case extension (including the dot) of the
 * last path component in `input`, after stripping any query string.
 * ext_out is set to "" if no extension is found. */
static void get_clean_extension(const char *input, char *ext_out, size_t ext_out_size) {
    const char *filename = strrchr(input, '/');
    filename = filename ? filename + 1 : input;

    /* Strip query string */
    char clean[512];
    const char *q = strchr(filename, '?');
    size_t copy_len = q ? (size_t)(q - filename) : strlen(filename);
    if (copy_len >= sizeof(clean)) copy_len = sizeof(clean) - 1;
    strncpy(clean, filename, copy_len);
    clean[copy_len] = '\0';

    const char *dot = strrchr(clean, '.');
    if (dot) {
        strncpy(ext_out, dot, ext_out_size - 1);
        ext_out[ext_out_size - 1] = '\0';
        /* Lower-case in place */
        for (char *p = ext_out; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;
    } else {
        ext_out[0] = '\0';
    }
}

/* Determine the MediaType for a given filename or URL.
 * Prefer the explicit `filename` parameter (Discord supplies it separately
 * from the CDN URL, without query strings), fall back to the URL. */
static MediaType detect_media_type(const char *filename, const char *url) {
    const char *probe = (filename && filename[0]) ? filename : url;
    if (!probe) return MEDIA_TYPE_OTHER;

    char ext[32];
    get_clean_extension(probe, ext, sizeof(ext));

    static const char *img_exts[] = {".jpg",".jpeg",".png",".gif",".webp",".bmp",NULL};
    for (int i = 0; img_exts[i]; i++)
        if (strcmp(ext, img_exts[i]) == 0) return MEDIA_TYPE_IMAGE;

    static const char *vid_exts[] = {".mp4",".webm",".mov",".avi",".mkv",".ogg",NULL};
    for (int i = 0; vid_exts[i]; i++)
        if (strcmp(ext, vid_exts[i]) == 0) return MEDIA_TYPE_VIDEO;

    return MEDIA_TYPE_OTHER;
}

/* Return the MIME type string for a given filename / URL extension. */
static const char *mime_for_video(const char *filename) {
    char ext[32];
    get_clean_extension(filename ? filename : "", ext, sizeof(ext));
    if (strcmp(ext, ".mp4")  == 0) return "video/mp4";
    if (strcmp(ext, ".webm") == 0) return "video/webm";
    if (strcmp(ext, ".ogg")  == 0) return "video/ogg";
    if (strcmp(ext, ".mov")  == 0) return "video/quicktime";
    return "video/mp4"; /* safe default */
}

/* -------------------------------------------------------------------------
 * Disk helpers
 * ---------------------------------------------------------------------- */

static void ensure_dir(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1)
        mkdir(path, 0755);
}

/* -------------------------------------------------------------------------
 * Download & save image (re-encoded as JPEG for compression)
 * Returns heap-allocated local path on success, NULL on failure.
 * ---------------------------------------------------------------------- */
static char *download_image(const char *url, int ticket_id, int attachment_id) {
    struct MemoryStruct chunk = {0};
    if (curl_download(url, &chunk) != 0 || chunk.size == 0) {
        free(chunk.memory);
        return NULL;
    }

    int width, height, channels;
    unsigned char *img = stbi_load_from_memory(chunk.memory, (int)chunk.size,
                                               &width, &height, &channels, 0);
    free(chunk.memory);
    if (!img) {
        fprintf(stderr, "[ticket] stbi_load_from_memory failed for %s\n", url);
        return NULL;
    }

    char ticket_dir[512];
    snprintf(ticket_dir, sizeof(ticket_dir), "%s/ticket_%d", TICKET_IMAGES_DIR, ticket_id);
    ensure_dir(TICKET_IMAGES_DIR);
    ensure_dir(ticket_dir);

    char *filepath = malloc(1024);
    snprintf(filepath, 1024, "%s/attachment_%d.jpg", ticket_dir, attachment_id);

    /* JPEG doesn't support alpha — convert RGBA→RGB */
    unsigned char *rgb = img;
    int write_channels = channels;
    if (channels == 4) {
        rgb = malloc(width * height * 3);
        for (int i = 0; i < width * height; i++) {
            rgb[i*3+0] = img[i*4+0];
            rgb[i*3+1] = img[i*4+1];
            rgb[i*3+2] = img[i*4+2];
        }
        write_channels = 3;
    }

    int ok = stbi_write_jpg(filepath, width, height, write_channels, rgb, 85);

    if (channels == 4) free(rgb);
    stbi_image_free(img);

    if (!ok) {
        fprintf(stderr, "[ticket] stbi_write_jpg failed: %s\n", filepath);
        free(filepath);
        return NULL;
    }

    printf("[ticket] Image saved: %s\n", filepath);
    return filepath;
}

/* -------------------------------------------------------------------------
 * Download & save a raw binary file (videos, non-image attachments)
 * Returns heap-allocated local path on success, NULL on failure.
 * ---------------------------------------------------------------------- */
static char *download_raw(const char *url, int ticket_id, int attachment_id,
                           const char *orig_filename) {
    struct MemoryStruct chunk = {0};
    if (curl_download(url, &chunk) != 0 || chunk.size == 0) {
        free(chunk.memory);
        return NULL;
    }

    char ticket_dir[512];
    snprintf(ticket_dir, sizeof(ticket_dir), "%s/ticket_%d", TICKET_IMAGES_DIR, ticket_id);
    ensure_dir(TICKET_IMAGES_DIR);
    ensure_dir(ticket_dir);

    /* Preserve original extension */
    char ext[32];
    get_clean_extension(orig_filename ? orig_filename : url, ext, sizeof(ext));
    if (!ext[0]) strncpy(ext, ".bin", sizeof(ext));

    char *filepath = malloc(1024);
    snprintf(filepath, 1024, "%s/attachment_%d%s", ticket_dir, attachment_id, ext);

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "[ticket] fopen failed: %s\n", filepath);
        free(chunk.memory);
        free(filepath);
        return NULL;
    }
    fwrite(chunk.memory, 1, chunk.size, f);
    fclose(f);
    free(chunk.memory);

    printf("[ticket] File saved: %s\n", filepath);
    return filepath;
}

/* -------------------------------------------------------------------------
 * Base64 encode a block of bytes.  Returns heap-allocated string.
 * ---------------------------------------------------------------------- */
static char *base64_encode(const unsigned char *data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t pos = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned char b0 = data[i];
        unsigned char b1 = (i+1 < len) ? data[i+1] : 0;
        unsigned char b2 = (i+2 < len) ? data[i+2] : 0;

        out[pos++] = tbl[b0 >> 2];
        out[pos++] = tbl[((b0 & 0x03) << 4) | (b1 >> 4)];
        out[pos++] = (i+1 < len) ? tbl[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
        out[pos++] = (i+2 < len) ? tbl[b2 & 0x3F]                       : '=';
    }
    out[pos] = '\0';
    return out;
}

/* Read a file from disk and return it base64-encoded.  Returns NULL on error. */
static char *file_to_base64(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }

    unsigned char *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, sz, f) != sz) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);

    char *b64 = base64_encode(buf, sz);
    free(buf);
    return b64;
}

/* -------------------------------------------------------------------------
 * Dynamic HTML buffer helpers
 * ---------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;   /* bytes currently written (excl. NUL) */
    size_t cap;   /* allocated capacity */
} HtmlBuf;

static int hbuf_init(HtmlBuf *h, size_t initial) {
    h->buf = malloc(initial);
    if (!h->buf) return -1;
    h->buf[0] = '\0';
    h->len = 0;
    h->cap = initial;
    return 0;
}

static int hbuf_append(HtmlBuf *h, const char *s) {
    size_t slen = strlen(s);
    while (h->len + slen + 1 > h->cap) {
        size_t new_cap = h->cap * 2;
        char *nb = realloc(h->buf, new_cap);
        if (!nb) return -1;
        h->buf = nb;
        h->cap = new_cap;
    }
    memcpy(h->buf + h->len, s, slen + 1);
    h->len += slen;
    return 0;
}

/* printf-style append to HtmlBuf — handles arbitrarily large formatted strings */
static int hbuf_appendf(HtmlBuf *h, const char *fmt, ...) {
    /* First pass: find out how much space we need */
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) return -1;

    /* Grow the buffer if necessary */
    while (h->len + (size_t)needed + 1 > h->cap) {
        size_t new_cap = h->cap * 2;
        if (new_cap < h->len + (size_t)needed + 1)
            new_cap = h->len + (size_t)needed + 1;
        char *nb = realloc(h->buf, new_cap);
        if (!nb) return -1;
        h->buf = nb;
        h->cap = new_cap;
    }

    /* Second pass: write directly into the buffer */
    va_start(ap, fmt);
    vsnprintf(h->buf + h->len, (size_t)needed + 1, fmt, ap);
    va_end(ap);

    h->len += (size_t)needed;
    return 0;
}

/* -------------------------------------------------------------------------
 * Database schema init + migration
 * ---------------------------------------------------------------------- */

static int init_ticket_tables(Database *db) {
    char *err_msg = NULL;

    /* Create tables — no-op if they already exist */
    int rc = sqlite3_exec(db->db, CREATE_TICKET_TABLES_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ticket] SQL error creating tables: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* -----------------------------------------------------------------------
     * Migration: the original schema had `is_image INTEGER DEFAULT 0`.
     * We renamed it to `media_type` (0=other, 1=image, 2=video).
     *
     * Because CREATE TABLE IF NOT EXISTS is a no-op on existing databases,
     * existing installs still have the OLD column name and the INSERT in
     * db_add_attachment() was failing silently (sqlite3_prepare_v2 returns
     * an error when the named column doesn't exist), causing attachment_id
     * to be -1 and skipping the download entirely.
     *
     * Fix:
     *   1. ADD media_type if it is missing (ignore "duplicate column" error).
     *   2. Back-fill it from is_image where present.
     * --------------------------------------------------------------------- */

    /* Step 1 – add the new column; silently ignore "duplicate column" */
    sqlite3_exec(db->db,
        "ALTER TABLE ticket_attachments ADD COLUMN media_type INTEGER DEFAULT 0",
        NULL, NULL, NULL);

    /* Step 2 – probe for the old is_image column via PRAGMA */
    {
        sqlite3_stmt *info_stmt;
        int has_is_image = 0;
        if (sqlite3_prepare_v2(db->db,
                "PRAGMA table_info(ticket_attachments)", -1, &info_stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(info_stmt) == SQLITE_ROW) {
                const char *col = (const char *)sqlite3_column_text(info_stmt, 1);
                if (col && strcmp(col, "is_image") == 0) { has_is_image = 1; break; }
            }
            sqlite3_finalize(info_stmt);
        }

        if (has_is_image) {
            sqlite3_exec(db->db,
                "UPDATE ticket_attachments "
                "SET media_type = 1 WHERE is_image = 1 AND media_type = 0",
                NULL, NULL, NULL);
            printf("[ticket] Migrated is_image -> media_type for existing rows\n");
        }
    }

    printf("[ticket] Ticket tables initialised\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Database operations
 * ---------------------------------------------------------------------- */

int db_set_server_config(Database *db, u64_snowflake_t main_guild_id,
                         u64_snowflake_t staff_guild_id,
                         u64_snowflake_t ticket_category_id,
                         u64_snowflake_t log_channel_id) {
    const char *sql =
        "INSERT OR REPLACE INTO server_configs "
        "(main_guild_id, staff_guild_id, ticket_category_id, log_channel_id) "
        "VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, main_guild_id);
    sqlite3_bind_int64(stmt, 2, staff_guild_id);
    sqlite3_bind_int64(stmt, 3, ticket_category_id);
    sqlite3_bind_int64(stmt, 4, log_channel_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_get_server_config(Database *db, u64_snowflake_t main_guild_id, ServerConfig *config) {
    const char *sql =
        "SELECT main_guild_id, staff_guild_id, ticket_category_id, log_channel_id "
        "FROM server_configs WHERE main_guild_id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, main_guild_id);
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        config->main_guild_id      = sqlite3_column_int64(stmt, 0);
        config->staff_guild_id     = sqlite3_column_int64(stmt, 1);
        config->ticket_category_id = sqlite3_column_int64(stmt, 2);
        config->log_channel_id     = sqlite3_column_int64(stmt, 3);
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found ? 0 : -1;
}

int db_create_ticket(Database *db, u64_snowflake_t user_id,
                     u64_snowflake_t main_guild_id,
                     u64_snowflake_t staff_guild_id,
                     u64_snowflake_t staff_channel_id,
                     u64_snowflake_t dm_channel_id) {
    const char *sql =
        "INSERT INTO tickets "
        "(user_id, main_guild_id, staff_guild_id, staff_channel_id, dm_channel_id, status) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, main_guild_id);
    sqlite3_bind_int64(stmt, 3, staff_guild_id);
    sqlite3_bind_int64(stmt, 4, staff_channel_id);
    sqlite3_bind_int64(stmt, 5, dm_channel_id);
    sqlite3_bind_int  (stmt, 6, TICKET_STATUS_OPEN);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[ticket] Failed to create ticket: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    return (int)sqlite3_last_insert_rowid(db->db);
}

int db_get_open_ticket_for_user(Database *db, u64_snowflake_t user_id, Ticket *ticket) {
    const char *sql =
        "SELECT id, user_id, main_guild_id, staff_guild_id, "
        "staff_channel_id, dm_channel_id, status, created_at, closed_at "
        "FROM tickets WHERE user_id = ? AND status = ? "
        "ORDER BY created_at DESC LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int  (stmt, 2, TICKET_STATUS_OPEN);
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ticket->id              = sqlite3_column_int  (stmt, 0);
        ticket->user_id         = sqlite3_column_int64(stmt, 1);
        ticket->main_guild_id   = sqlite3_column_int64(stmt, 2);
        ticket->staff_guild_id  = sqlite3_column_int64(stmt, 3);
        ticket->staff_channel_id= sqlite3_column_int64(stmt, 4);
        ticket->dm_channel_id   = sqlite3_column_int64(stmt, 5);
        ticket->status          = sqlite3_column_int  (stmt, 6);
        ticket->created_at      = sqlite3_column_int64(stmt, 7);
        ticket->closed_at       = sqlite3_column_int64(stmt, 8);
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found ? 0 : -1;
}

int db_get_ticket_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *ticket) {
    const char *sql =
        "SELECT id, user_id, main_guild_id, staff_guild_id, "
        "staff_channel_id, dm_channel_id, status, created_at, closed_at "
        "FROM tickets WHERE (staff_channel_id = ? OR dm_channel_id = ?) AND status = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, channel_id);
    sqlite3_bind_int64(stmt, 2, channel_id);
    sqlite3_bind_int  (stmt, 3, TICKET_STATUS_OPEN);
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ticket->id              = sqlite3_column_int  (stmt, 0);
        ticket->user_id         = sqlite3_column_int64(stmt, 1);
        ticket->main_guild_id   = sqlite3_column_int64(stmt, 2);
        ticket->staff_guild_id  = sqlite3_column_int64(stmt, 3);
        ticket->staff_channel_id= sqlite3_column_int64(stmt, 4);
        ticket->dm_channel_id   = sqlite3_column_int64(stmt, 5);
        ticket->status          = sqlite3_column_int  (stmt, 6);
        ticket->created_at      = sqlite3_column_int64(stmt, 7);
        ticket->closed_at       = sqlite3_column_int64(stmt, 8);
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found ? 0 : -1;
}

int db_close_ticket(Database *db, int ticket_id) {
    const char *sql =
        "UPDATE tickets SET status = ?, closed_at = strftime('%s', 'now') WHERE id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, TICKET_STATUS_CLOSED);
    sqlite3_bind_int(stmt, 2, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_add_ticket_message(Database *db, int ticket_id, u64_snowflake_t message_id,
                          u64_snowflake_t author_id, const char *content,
                          const char *attachments_json, bool from_user) {
    const char *sql =
        "INSERT INTO ticket_messages "
        "(ticket_id, message_id, author_id, content, attachments_json, from_user) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (stmt, 1, ticket_id);
    sqlite3_bind_int64(stmt, 2, message_id);
    sqlite3_bind_int64(stmt, 3, author_id);
    sqlite3_bind_text (stmt, 4, content,          -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 5, attachments_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, 6, from_user ? 1 : 0);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? (int)sqlite3_last_insert_rowid(db->db) : -1;
}

int db_add_attachment(Database *db, int message_db_id, const char *url,
                      const char *filename, int media_type) {
    const char *sql =
        "INSERT INTO ticket_attachments "
        "(message_id, original_url, filename, media_type) VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int (stmt, 1, message_db_id);
    sqlite3_bind_text(stmt, 2, url,      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, filename, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 4, media_type);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? (int)sqlite3_last_insert_rowid(db->db) : -1;
}

int db_update_attachment_path(Database *db, int attachment_id, const char *local_path) {
    const char *sql =
        "UPDATE ticket_attachments SET local_path = ?, download_status = 1 WHERE id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, local_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, attachment_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_ticket_message(Database *db, u64_snowflake_t message_id, const char *new_content) {
    const char *sql =
        "UPDATE ticket_messages SET content = ?, is_edited = 1, "
        "edited_at = strftime('%s', 'now') WHERE message_id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (stmt, 1, new_content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, message_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_delete_ticket_message(Database *db, u64_snowflake_t message_id) {
    const char *sql = "UPDATE ticket_messages SET is_deleted = 1 WHERE message_id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, message_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_get_ticket_messages(Database *db, int ticket_id,
                            TicketMessage **messages, int *count) {
    const char *sql =
        "SELECT id, ticket_id, message_id, author_id, content, attachments_json, "
        "from_user, is_deleted, is_edited, timestamp, edited_at "
        "FROM ticket_messages WHERE ticket_id = ? ORDER BY timestamp ASC";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, ticket_id);

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    *count = row_count;

    if (row_count == 0) { sqlite3_finalize(stmt); *messages = NULL; return 0; }

    *messages = malloc(sizeof(TicketMessage) * row_count);
    if (!*messages) { sqlite3_finalize(stmt); return -1; }

    sqlite3_reset(stmt);
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*messages)[idx].id        = sqlite3_column_int  (stmt, 0);
        (*messages)[idx].ticket_id = sqlite3_column_int  (stmt, 1);
        (*messages)[idx].message_id= sqlite3_column_int64(stmt, 2);
        (*messages)[idx].author_id = sqlite3_column_int64(stmt, 3);

        const char *c = (const char *)sqlite3_column_text(stmt, 4);
        (*messages)[idx].content = c ? strdup(c) : NULL;

        const char *a = (const char *)sqlite3_column_text(stmt, 5);
        (*messages)[idx].attachments_json = a ? strdup(a) : NULL;

        (*messages)[idx].from_user  = sqlite3_column_int (stmt, 6) != 0;
        (*messages)[idx].is_deleted = sqlite3_column_int (stmt, 7) != 0;
        (*messages)[idx].is_edited  = sqlite3_column_int (stmt, 8) != 0;
        (*messages)[idx].timestamp  = sqlite3_column_int64(stmt, 9);
        (*messages)[idx].edited_at  = sqlite3_column_int64(stmt, 10);
        idx++;
    }
    sqlite3_finalize(stmt);
    return 0;
}

void db_free_ticket_messages(TicketMessage *messages, int count) {
    if (!messages) return;
    for (int i = 0; i < count; i++) {
        free(messages[i].content);
        free(messages[i].attachments_json);
    }
    free(messages);
}

/* -------------------------------------------------------------------------
 * Attachment retrieval (internal)
 * ---------------------------------------------------------------------- */

typedef struct {
    int   id;
    char *original_url;
    char *local_path;
    char *filename;
    int   media_type;       /* MediaType enum */
    int   download_status;
} MessageAttachment;

static int db_get_message_attachments(Database *db, int message_db_id,
                                       MessageAttachment **out, int *count) {
    const char *sql =
        "SELECT id, original_url, local_path, filename, media_type, download_status "
        "FROM ticket_attachments WHERE message_id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, message_db_id);

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    *count = row_count;

    if (row_count == 0) { sqlite3_finalize(stmt); *out = NULL; return 0; }

    *out = malloc(sizeof(MessageAttachment) * row_count);
    if (!*out) { sqlite3_finalize(stmt); return -1; }

    sqlite3_reset(stmt);
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*out)[idx].id = sqlite3_column_int(stmt, 0);

        const char *u = (const char *)sqlite3_column_text(stmt, 1);
        (*out)[idx].original_url = u ? strdup(u) : NULL;

        const char *p = (const char *)sqlite3_column_text(stmt, 2);
        (*out)[idx].local_path = p ? strdup(p) : NULL;

        const char *f = (const char *)sqlite3_column_text(stmt, 3);
        (*out)[idx].filename = f ? strdup(f) : NULL;

        (*out)[idx].media_type      = sqlite3_column_int(stmt, 4);
        (*out)[idx].download_status = sqlite3_column_int(stmt, 5);
        idx++;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static void db_free_message_attachments(MessageAttachment *atts, int count) {
    if (!atts) return;
    for (int i = 0; i < count; i++) {
        free(atts[i].original_url);
        free(atts[i].local_path);
        free(atts[i].filename);
    }
    free(atts);
}

/* -------------------------------------------------------------------------
 * HTML generation
 *
 * Images are embedded as data:image/jpeg;base64,...
 * Videos are embedded as data:<mime>;base64,...  inside a <video> element.
 * Other attachments fall back to a styled external link.
 * ---------------------------------------------------------------------- */

/* HTML-escape a string into a malloc'd buffer. */
static char *html_escape(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len * 6 + 1);
    char *p = dst;
    while (*src) {
        switch (*src) {
            case '<':  memcpy(p, "&lt;",   4); p += 4; break;
            case '>':  memcpy(p, "&gt;",   4); p += 4; break;
            case '&':  memcpy(p, "&amp;",  5); p += 5; break;
            case '"':  memcpy(p, "&quot;", 6); p += 6; break;
            case '\'': memcpy(p, "&#39;",  5); p += 5; break;
            default:   *p++ = *src; break;
        }
        src++;
    }
    *p = '\0';
    return dst;
}

/* Append the HTML for a single attachment to hbuf. */
static void append_attachment_html(HtmlBuf *h, const MessageAttachment *att) {
    /* Try to embed from local file first */
    if (att->local_path && att->download_status == 1) {
        char *b64 = file_to_base64(att->local_path);
        if (b64) {
            if (att->media_type == MEDIA_TYPE_IMAGE) {
                hbuf_appendf(h,
                    "                    <img "
                    "src=\"data:image/jpeg;base64,%s\" "
                    "class=\"attachment-image\" "
                    "alt=\"Attachment\" "
                    "onclick=\"window.open('%s','_blank')\">\n",
                    b64, att->original_url ? att->original_url : "#");
            } else if (att->media_type == MEDIA_TYPE_VIDEO) {
                const char *mime = mime_for_video(att->filename ? att->filename : att->local_path);
                hbuf_appendf(h,
                    "                    <video controls class=\"attachment-video\">\n"
                    "                        <source src=\"data:%s;base64,%s\" type=\"%s\">\n"
                    "                        <a href=\"%s\" class=\"attachment-link\" "
                    "target=\"_blank\">%s (video — browser can't play inline)</a>\n"
                    "                    </video>\n",
                    mime, b64, mime,
                    att->original_url ? att->original_url : "#",
                    att->filename     ? att->filename     : "Video");
            }
            free(b64);
            return;
        }
        /* If file_to_base64 failed, fall through to link */
    }

    /* Fallback: external link */
    const char *display = att->filename ? att->filename : att->original_url;
    const char *url     = att->original_url ? att->original_url : "#";
    hbuf_appendf(h,
        "                    <a href=\"%s\" class=\"attachment-link\" target=\"_blank\">%s</a>\n",
        url, display ? display : "Attachment");
}

char *generate_ticket_html(Database *db, int ticket_id, const char *username) {
    TicketMessage *messages = NULL;
    int count = 0;
    if (db_get_ticket_messages(db, ticket_id, &messages, &count) != 0) return NULL;

    HtmlBuf h;
    /* Start with 4 MB; hbuf_append will grow as needed */
    if (hbuf_init(&h, 4 * 1024 * 1024) != 0) {
        db_free_ticket_messages(messages, count);
        return NULL;
    }

    /* ---- Header ---- */
    hbuf_appendf(&h,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"UTF-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
        "  <title>Ticket #%d — %s</title>\n"
        "  <style>\n"
        "    *{margin:0;padding:0;box-sizing:border-box}\n"
        "    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
                  "background:#1a1d21;color:#dcddde;padding:20px;line-height:1.5}\n"
        "    .container{max-width:1200px;margin:0 auto;background:#2f3136;"
                       "border-radius:8px;overflow:hidden}\n"
        "    .header{background:#202225;padding:24px 32px;border-bottom:1px solid #1a1d21}\n"
        "    .header h1{font-size:24px;font-weight:600;color:#fff;margin-bottom:8px}\n"
        "    .header .meta{font-size:14px;color:#b9bbbe}\n"
        "    .header .meta span{margin-right:16px}\n"
        "    .messages{padding:24px 32px}\n"
        "    .message-group{margin-bottom:20px;padding:4px 8px;border-radius:4px}\n"
        "    .message-group:hover{background:#32353b}\n"
        "    .message-author{display:flex;align-items:baseline;margin-bottom:4px}\n"
        "    .author-name{font-weight:500;font-size:15px;margin-right:8px}\n"
        "    .message-group.user  .author-name{color:#5865f2}\n"
        "    .message-group.staff .author-name{color:#ed4245}\n"
        "    .author-badge{font-size:10px;font-weight:600;padding:2px 4px;"
                          "border-radius:3px;text-transform:uppercase;margin-right:8px;color:#fff}\n"
        "    .message-group.user  .author-badge{background:#5865f2}\n"
        "    .message-group.staff .author-badge{background:#ed4245}\n"
        "    .timestamp{font-size:12px;color:#72767d}\n"
        "    .message-content{color:#dcddde;font-size:15px;margin-bottom:4px;"
                             "white-space:pre-wrap;word-wrap:break-word}\n"
        "    .message-content.deleted{color:#72767d;font-style:italic;text-decoration:line-through}\n"
        "    .edit-indicator{color:#72767d;font-size:11px;margin-left:4px}\n"
        "    .attachments{margin-top:8px}\n"
        "    .attachment-image{max-width:400px;max-height:400px;border-radius:4px;"
                              "margin:8px 0;cursor:pointer;transition:transform .2s;display:block}\n"
        "    .attachment-image:hover{transform:scale(1.02)}\n"
        "    .attachment-video{max-width:560px;border-radius:4px;margin:8px 0;display:block}\n"
        "    .attachment-link{color:#00a8fc;text-decoration:none;font-size:14px;"
                             "display:block;margin:4px 0;padding:8px;background:#202225;"
                             "border-radius:4px;border-left:2px solid #5865f2}\n"
        "    .attachment-link:hover{background:#292b2f;text-decoration:underline}\n"
        "    .attachment-link::before{content:'📎 '}\n"
        "    .footer{background:#202225;padding:16px 32px;border-top:1px solid #1a1d21;"
                   "text-align:center;color:#72767d;font-size:12px}\n"
        "    .internal-note{background:#faa81a;color:#000;padding:12px;border-radius:4px;"
                           "margin-bottom:16px;font-size:14px;font-weight:500}\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"container\">\n"
        "    <div class=\"header\">\n"
        "      <h1>Ticket #%d</h1>\n"
        "      <div class=\"meta\">\n"
        "        <span><strong>User:</strong> %s</span>\n"
        "        <span><strong>Messages:</strong> %d</span>\n"
        "        <span><strong>Status:</strong> Closed</span>\n"
        "      </div>\n"
        "    </div>\n"
        "    <div class=\"messages\">\n"
        "      <div class=\"internal-note\">⚠️ STAFF ONLY — Internal Ticket Transcript</div>\n",
        ticket_id, username,
        ticket_id, username, count);

    /* ---- Messages ---- */
    for (int i = 0; i < count; i++) {
        bool is_user = messages[i].from_user;

        bool start_group = (i == 0)
            || (messages[i-1].from_user != is_user)
            || (!is_user && messages[i-1].author_id != messages[i].author_id);

        if (start_group) {
            time_t ts = (time_t)messages[i].timestamp;
            struct tm *tm = gmtime(&ts);
            char time_buf[64];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", tm);

            char author_name[128];
            if (is_user) {
                snprintf(author_name, sizeof(author_name), "%s", username);
            } else {
                struct discord_user staff_info = {0};
                discord_get_user(g_client, messages[i].author_id, &staff_info);
                if (staff_info.username && staff_info.username[0])
                    snprintf(author_name, sizeof(author_name), "%s", staff_info.username);
                else
                    snprintf(author_name, sizeof(author_name),
                             "Staff (ID: %" PRIu64 ")", messages[i].author_id);
                discord_user_cleanup(&staff_info);
            }

            hbuf_appendf(&h,
                "      <div class=\"message-group %s\">\n"
                "        <div class=\"message-author\">\n"
                "          <span class=\"author-badge\">%s</span>\n"
                "          <span class=\"author-name\">%s</span>\n"
                "          <span class=\"timestamp\">%s</span>\n"
                "        </div>\n",
                is_user ? "user" : "staff",
                is_user ? "USER" : "STAFF",
                author_name,
                time_buf);
        }

        /* Message content */
        if (messages[i].is_deleted) {
            hbuf_append(&h, "        <div class=\"message-content deleted\">[Message Deleted]</div>\n");
        } else {
            char *esc = html_escape(messages[i].content ? messages[i].content : "");
            hbuf_append(&h, "        <div class=\"message-content\">");
            hbuf_append(&h, esc);
            free(esc);
            if (messages[i].is_edited)
                hbuf_append(&h, "<span class=\"edit-indicator\">(edited)</span>");
            hbuf_append(&h, "</div>\n");
        }

        /* Attachments */
        MessageAttachment *atts = NULL;
        int att_count = 0;
        if (db_get_message_attachments(db, messages[i].id, &atts, &att_count) == 0
                && att_count > 0) {
            hbuf_append(&h, "        <div class=\"attachments\">\n");
            for (int j = 0; j < att_count; j++)
                append_attachment_html(&h, &atts[j]);
            hbuf_append(&h, "        </div>\n");
            db_free_message_attachments(atts, att_count);
        }

        /* Close group? */
        bool end_group = (i == count - 1)
            || (messages[i+1].from_user != is_user)
            || (!is_user && messages[i+1].author_id != messages[i].author_id);
        if (end_group)
            hbuf_append(&h, "      </div>\n");
    }

    /* ---- Footer ---- */
    hbuf_append(&h,
        "    </div>\n"
        "    <div class=\"footer\">Internal Staff Transcript | Confidential</div>\n"
        "  </div>\n"
        "</body>\n"
        "</html>\n");

    db_free_ticket_messages(messages, count);
    return h.buf;   /* caller owns this */
}

/* -------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */

static void handle_ticket_create(struct discord *client,
                                  const struct discord_interaction *event) {
    if (event->guild_id != 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ This command can only be used in DMs with the bot!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    Ticket existing;
    if (db_get_open_ticket_for_user(g_db, event->user->id, &existing) == 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ You already have an open ticket! Please close it before creating a new one.",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    const char *guild_id_str = NULL;
    if (event->data && event->data->options) {
        for (int i = 0; event->data->options[i]; i++) {
            if (strcmp(event->data->options[i]->name, "server") == 0) {
                guild_id_str = event->data->options[i]->value;
                break;
            }
        }
    }

    if (!guild_id_str) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Server ID not provided!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    u64_snowflake_t main_guild_id = (u64_snowflake_t)strtoull(guild_id_str, NULL, 10);

    ServerConfig config;
    if (db_get_server_config(g_db, main_guild_id, &config) != 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ This server doesn't have ticket support configured!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    struct discord_create_dm_params dm_params = { .recipient_id = event->user->id };
    struct discord_channel dm_channel = {0};

    if (discord_create_dm(client, &dm_params, &dm_channel) != ORCA_OK) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to create DM channel!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    char channel_name[64];
    snprintf(channel_name, sizeof(channel_name), "ticket-%s", event->user->username);

    struct discord_create_guild_channel_params channel_params = {
        .name      = channel_name,
        .type      = DISCORD_CHANNEL_GUILD_TEXT,
        .parent_id = config.ticket_category_id,
    };

    struct discord_channel staff_channel = {0};
    if (discord_create_guild_channel(client, config.staff_guild_id, &channel_params, &staff_channel) != ORCA_OK) {
        discord_channel_cleanup(&dm_channel);
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to create staff channel!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    int ticket_id = db_create_ticket(g_db, event->user->id, main_guild_id,
                                     config.staff_guild_id, staff_channel.id, dm_channel.id);
    if (ticket_id < 0) {
        discord_channel_cleanup(&dm_channel);
        discord_channel_cleanup(&staff_channel);
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to create ticket in database!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    char staff_msg[512];
    snprintf(staff_msg, sizeof(staff_msg),
             "🎫 **New Ticket #%d**\n\n"
             "**User:** %s (ID: %" PRIu64 ")\n"
             "**Server:** %" PRIu64 "\n\n"
             "All messages in this channel will be relayed to the user anonymously.\n"
             "Use `/closeticket` to close this ticket.",
             ticket_id, event->user->username, event->user->id, main_guild_id);

    struct discord_create_message_params sp = { .content = staff_msg };
    discord_create_message(client, staff_channel.id, &sp, NULL);

    char user_msg[256];
    snprintf(user_msg, sizeof(user_msg),
             "✅ **Ticket Created!**\n\n"
             "Your ticket #%d has been created. Staff members will respond to you here.\n"
             "All messages you send here will be forwarded to staff anonymously.",
             ticket_id);

    struct discord_create_message_params up = { .content = user_msg };
    discord_create_message(client, dm_channel.id, &up, NULL);

    char response_text[128];
    snprintf(response_text, sizeof(response_text),
             "✅ Ticket #%d created! Check your DMs to continue.", ticket_id);

    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response_text,
            .flags   = 1 << 6,
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &response, NULL);

    discord_channel_cleanup(&dm_channel);
    discord_channel_cleanup(&staff_channel);
}

static void handle_ticket_close(struct discord *client,
                                 const struct discord_interaction *event) {
    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, event->channel_id, &ticket) != 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ This is not a ticket channel!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    if (db_close_ticket(g_db, ticket.id) != 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to close ticket!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    struct discord_user user_info = {0};
    discord_get_user(client, ticket.user_id, &user_info);
    const char *uname = user_info.username ? user_info.username : "Unknown User";

    char *html = generate_ticket_html(g_db, ticket.id, uname);
    if (html) {
        char filename[128];
        snprintf(filename, sizeof(filename), "ticket_%d.html", ticket.id);

        FILE *f = fopen(filename, "w");
        if (f) {
            fprintf(f, "%s", html);
            fclose(f);

            ServerConfig config;
            if (db_get_server_config(g_db, ticket.main_guild_id, &config) == 0) {
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg),
                         "📋 **Ticket #%d Closed**\n"
                         "**Closed by:** <@%" PRIu64 ">\n"
                         "**User:** %s (%" PRIu64 ")\n"
                         "**Transcript:** See attached HTML file",
                         ticket.id,
                         event->member ? event->member->user->id : 0,
                         uname, ticket.user_id);

                struct discord_attachment attachment = { .filename = filename };
                struct discord_attachment *attachments[] = { &attachment, NULL };
                struct discord_create_message_params log_params = {
                    .content     = log_msg,
                    .attachments = attachments,
                };
                discord_create_message(client, config.log_channel_id, &log_params, NULL);
            }
            remove(filename);
        }
        free(html);
    }

    char user_msg[128];
    snprintf(user_msg, sizeof(user_msg),
             "🔒 Your ticket #%d has been closed by staff.\nThank you for contacting us!",
             ticket.id);
    struct discord_create_message_params up = { .content = user_msg };
    discord_create_message(client, ticket.dm_channel_id, &up, NULL);

    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = "✅ Ticket closed! This channel will be deleted in 10 seconds.",
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &response, NULL);

    discord_delete_channel(client, event->channel_id, NULL);
    discord_user_cleanup(&user_info);
}

static void handle_config_set(struct discord *client,
                               const struct discord_interaction *event) {
    const char *main_guild_str  = NULL;
    const char *staff_guild_str = NULL;
    const char *category_str    = NULL;
    const char *log_channel_str = NULL;

    if (event->data && event->data->options) {
        for (int i = 0; event->data->options[i]; i++) {
            const char *name  = event->data->options[i]->name;
            const char *value = event->data->options[i]->value;
            if (strcmp(name, "main_server")  == 0) main_guild_str  = value;
            else if (strcmp(name, "staff_server") == 0) staff_guild_str = value;
            else if (strcmp(name, "category")     == 0) category_str    = value;
            else if (strcmp(name, "log_channel")  == 0) log_channel_str = value;
        }
    }

    if (!main_guild_str || !staff_guild_str || !category_str || !log_channel_str) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Missing required parameters!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    u64_snowflake_t main_guild  = (u64_snowflake_t)strtoull(main_guild_str,  NULL, 10);
    u64_snowflake_t staff_guild = (u64_snowflake_t)strtoull(staff_guild_str, NULL, 10);
    u64_snowflake_t category    = (u64_snowflake_t)strtoull(category_str,    NULL, 10);
    u64_snowflake_t log_channel = (u64_snowflake_t)strtoull(log_channel_str, NULL, 10);

    if (db_set_server_config(g_db, main_guild, staff_guild, category, log_channel) != 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ Failed to save configuration!",
                .flags = 1 << 6,
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }

    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = "✅ Ticket system configured successfully!",
            .flags = 1 << 6,
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &response, NULL);
}

/* -------------------------------------------------------------------------
 * Message event handlers
 * ---------------------------------------------------------------------- */

void on_ticket_message(struct discord *client, const struct discord_message *event) {
    if (event->author && event->author->bot) return;

    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, event->channel_id, &ticket) != 0) return;

    bool from_user = (event->channel_id == ticket.dm_channel_id);

    /* Build compact attachments JSON for the message record */
    char *attachments_json = NULL;
    if (event->attachments && event->attachments[0]) {
        size_t json_size = 8192;
        attachments_json = malloc(json_size);
        snprintf(attachments_json, json_size, "[");
        for (int i = 0; event->attachments[i]; i++) {
            if (i > 0) strncat(attachments_json, ",", json_size - strlen(attachments_json) - 1);
            if (event->attachments[i]->url) {
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "\"%s\"", event->attachments[i]->url);
                strncat(attachments_json, tmp, json_size - strlen(attachments_json) - 1);
            }
        }
        strncat(attachments_json, "]", json_size - strlen(attachments_json) - 1);
    } else {
        attachments_json = strdup("[]");
    }

    int message_db_id = db_add_ticket_message(g_db, ticket.id, event->id,
                                               event->author->id,
                                               event->content ? event->content : "",
                                               attachments_json, from_user);
    free(attachments_json);

    /* Download and store each attachment */
    if (message_db_id > 0 && event->attachments && event->attachments[0]) {
        for (int i = 0; event->attachments[i]; i++) {
            struct discord_attachment *att = event->attachments[i];
            if (!att->url) continue;

            /* Prefer att->filename (no query string) for type detection */
            const char *fname    = att->filename ? att->filename : "";
            MediaType   mtype    = detect_media_type(fname, att->url);
            const char *db_fname = *fname ? fname : att->url;

            printf("[ticket] Attachment %d: fname='%s' url='%.80s...' type=%d\n",
                   i, fname, att->url, (int)mtype);

            int attachment_id = db_add_attachment(g_db, message_db_id,
                                                   att->url, db_fname, (int)mtype);
            if (attachment_id <= 0) {
                fprintf(stderr, "[ticket] db_add_attachment failed for '%s' "
                                "(message_db_id=%d, sqlite err: %s)\n",
                        db_fname, message_db_id, sqlite3_errmsg(g_db->db));
                continue;
            }

            printf("[ticket] Recorded attachment id=%d, downloading...\n", attachment_id);

            char *local_path = NULL;
            if (mtype == MEDIA_TYPE_IMAGE) {
                local_path = download_image(att->url, ticket.id, attachment_id);
            } else if (mtype == MEDIA_TYPE_VIDEO) {
                local_path = download_raw(att->url, ticket.id, attachment_id, fname);
            } else {
                printf("[ticket] Attachment is not image/video — keeping as link\n");
            }

            if (local_path) {
                db_update_attachment_path(g_db, attachment_id, local_path);
                printf("[ticket] Saved to %s\n", local_path);
                free(local_path);
            } else if (mtype == MEDIA_TYPE_IMAGE || mtype == MEDIA_TYPE_VIDEO) {
                fprintf(stderr, "[ticket] Download failed for attachment id=%d url=%.80s\n",
                        attachment_id, att->url);
            }
        }
    } else if (message_db_id <= 0) {
        fprintf(stderr, "[ticket] db_add_ticket_message failed — skipping attachment download\n");
    }

    /* Forward the message to the other side */
    u64_snowflake_t target = from_user ? ticket.staff_channel_id : ticket.dm_channel_id;

    char forwarded[2048];
    const char *content = event->content ? event->content : "";
    if (from_user)
        snprintf(forwarded, sizeof(forwarded), "**%s:** %s", event->author->username, content);
    else
        snprintf(forwarded, sizeof(forwarded), "**Staff:** %s", content);

    if (event->attachments && event->attachments[0]) {
        strncat(forwarded, "\n📎 Attachments:", sizeof(forwarded) - strlen(forwarded) - 1);
        for (int i = 0; event->attachments[i]; i++) {
            if (event->attachments[i]->url) {
                char line[256];
                snprintf(line, sizeof(line), "\n• %s", event->attachments[i]->url);
                strncat(forwarded, line, sizeof(forwarded) - strlen(forwarded) - 1);
            }
        }
    }

    struct discord_create_message_params params = { .content = forwarded };
    discord_create_message(client, target, &params, NULL);
}

void on_ticket_message_update(struct discord *client, const struct discord_message *event) {
    if (event->author && event->author->bot) return;

    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, event->channel_id, &ticket) != 0) return;

    if (event->content)
        db_update_ticket_message(g_db, event->id, event->content);

    bool from_user = (event->channel_id == ticket.dm_channel_id);
    u64_snowflake_t target = from_user ? ticket.staff_channel_id : ticket.dm_channel_id;

    char notice[2048];
    if (from_user)
        snprintf(notice, sizeof(notice), "✏️ **%s** edited their message:\n**New content:** %s",
                 event->author ? event->author->username : "User",
                 event->content ? event->content : "(no content)");
    else
        snprintf(notice, sizeof(notice), "✏️ **Staff** edited their message:\n**New content:** %s",
                 event->content ? event->content : "(no content)");

    struct discord_create_message_params params = { .content = notice };
    discord_create_message(client, target, &params, NULL);
}

void on_ticket_message_delete(struct discord *client,
                               u64_snowflake_t message_id, u64_snowflake_t channel_id) {
    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, channel_id, &ticket) != 0) return;

    db_delete_ticket_message(g_db, message_id);

    bool from_user = (channel_id == ticket.dm_channel_id);
    u64_snowflake_t target = from_user ? ticket.staff_channel_id : ticket.dm_channel_id;

    const char *notice = from_user ? "🗑️ User deleted a message" : "🗑️ Staff deleted a message";
    struct discord_create_message_params params = { .content = (char *)notice };
    discord_create_message(client, target, &params, NULL);
}

/* -------------------------------------------------------------------------
 * Interaction router
 * ---------------------------------------------------------------------- */

void on_ticket_interaction(struct discord *client, const struct discord_interaction *event) {
    if (!event->data) return;
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;

    const char *cmd = event->data->name;

    /* Diagnostic: always log what Discord actually sent */
    printf("[ticket] on_ticket_interaction: cmd='%s' guild_id=%" PRIu64 "\n",
           cmd ? cmd : "(null)", event->guild_id);

    if (!cmd) return;

    if      (strcmp(cmd, "ticket")       == 0) handle_ticket_create(client, event);
    else if (strcmp(cmd, "closeticket")  == 0) handle_ticket_close (client, event);
    else if (strcmp(cmd, "ticketconfig") == 0) handle_config_set   (client, event);
}

/* -------------------------------------------------------------------------
 * Slash command registration
 * ---------------------------------------------------------------------- */

void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id) {
    /* /ticket — global, intended for DMs */
    struct discord_application_command_option server_opt = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "server",
        .description = "The server ID you want to contact staff for",
        .required    = true,
    };
    struct discord_application_command_option *ticket_opts[] = { &server_opt, NULL };
    struct discord_create_global_application_command_params ticket_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "ticket",
        .description = "Create a support ticket (use in DMs)",
        .options     = ticket_opts,
    };
    discord_create_global_application_command(client, application_id, &ticket_params, NULL);

    /* /closeticket — guild-scoped */
    struct discord_create_guild_application_command_params close_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "closeticket",
        .description = "Close the current ticket",
    };
    discord_create_guild_application_command(client, application_id, guild_id, &close_params, NULL);

    /* /ticketconfig — guild-scoped */
    struct discord_application_command_option main_server  = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "main_server", .description = "Main server ID (where users are)", .required = true
    };
    struct discord_application_command_option staff_server = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "staff_server", .description = "Staff server ID (where tickets are created)", .required = true
    };
    struct discord_application_command_option category = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "category", .description = "Category ID for ticket channels", .required = true
    };
    struct discord_application_command_option log_channel = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "log_channel", .description = "Channel ID for ticket logs", .required = true
    };
    struct discord_application_command_option *config_opts[] = {
        &main_server, &staff_server, &category, &log_channel, NULL
    };
    struct discord_create_guild_application_command_params config_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "ticketconfig",
        .description = "Configure ticket system (Admin only)",
        .options     = config_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id, &config_params, NULL);

    printf("[ticket] Ticket commands registered\n");
}

/* -------------------------------------------------------------------------
 * Module init / cleanup
 * ---------------------------------------------------------------------- */

void ticket_module_init(struct discord *client, Database *db) {
    g_db     = db;
    g_client = client;

    curl_global_init(CURL_GLOBAL_ALL);
    ensure_dir(TICKET_IMAGES_DIR);
    init_ticket_tables(db);

    printf("[ticket] Ticket module initialised with embedded media support\n");
}

void ticket_module_cleanup(void) {
    curl_global_cleanup();
}