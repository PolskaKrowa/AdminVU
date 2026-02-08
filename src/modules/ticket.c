#include "ticket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

// Module-private state
static Database *g_db = NULL;
static struct discord *g_client = NULL;

// SQL schema for ticket tables
static const char *CREATE_TICKET_TABLES_SQL = 
    "CREATE TABLE IF NOT EXISTS server_configs ("
    "    main_guild_id INTEGER PRIMARY KEY,"
    "    staff_guild_id INTEGER NOT NULL,"
    "    ticket_category_id INTEGER NOT NULL,"
    "    log_channel_id INTEGER NOT NULL"
    ");"
    ""
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
    ""
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
    ""
    "CREATE INDEX IF NOT EXISTS idx_message_id ON ticket_messages(message_id);";

// Initialise ticket database tables
static int init_ticket_tables(Database *db) {
    char *err_msg = NULL;
    int rc = sqlite3_exec(db->db, CREATE_TICKET_TABLES_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error creating ticket tables: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    printf("[ticket] Ticket tables initialised\n");
    return 0;
}

// Database operations
int db_set_server_config(Database *db, u64_snowflake_t main_guild_id,
                         u64_snowflake_t staff_guild_id,
                         u64_snowflake_t ticket_category_id,
                         u64_snowflake_t log_channel_id) {
    const char *sql = "INSERT OR REPLACE INTO server_configs "
                      "(main_guild_id, staff_guild_id, ticket_category_id, log_channel_id) "
                      "VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, main_guild_id);
    sqlite3_bind_int64(stmt, 2, staff_guild_id);
    sqlite3_bind_int64(stmt, 3, ticket_category_id);
    sqlite3_bind_int64(stmt, 4, log_channel_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_get_server_config(Database *db, u64_snowflake_t main_guild_id,
                         ServerConfig *config) {
    const char *sql = "SELECT main_guild_id, staff_guild_id, ticket_category_id, log_channel_id "
                      "FROM server_configs WHERE main_guild_id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, main_guild_id);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        config->main_guild_id = sqlite3_column_int64(stmt, 0);
        config->staff_guild_id = sqlite3_column_int64(stmt, 1);
        config->ticket_category_id = sqlite3_column_int64(stmt, 2);
        config->log_channel_id = sqlite3_column_int64(stmt, 3);
        sqlite3_finalize(stmt);
        return 0;
    }
    
    sqlite3_finalize(stmt);
    return -1;
}

int db_create_ticket(Database *db, u64_snowflake_t user_id,
                     u64_snowflake_t main_guild_id,
                     u64_snowflake_t staff_guild_id,
                     u64_snowflake_t staff_channel_id,
                     u64_snowflake_t dm_channel_id) {
    const char *sql = "INSERT INTO tickets "
                      "(user_id, main_guild_id, staff_guild_id, staff_channel_id, dm_channel_id, status) "
                      "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, main_guild_id);
    sqlite3_bind_int64(stmt, 3, staff_guild_id);
    sqlite3_bind_int64(stmt, 4, staff_channel_id);
    sqlite3_bind_int64(stmt, 5, dm_channel_id);
    sqlite3_bind_int(stmt, 6, TICKET_STATUS_OPEN);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to create ticket: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    return sqlite3_last_insert_rowid(db->db);
}

int db_get_open_ticket_for_user(Database *db, u64_snowflake_t user_id, Ticket *ticket) {
    const char *sql = "SELECT id, user_id, main_guild_id, staff_guild_id, "
                      "staff_channel_id, dm_channel_id, status, created_at, closed_at "
                      "FROM tickets WHERE user_id = ? AND status = ? "
                      "ORDER BY created_at DESC LIMIT 1";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, TICKET_STATUS_OPEN);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ticket->id = sqlite3_column_int(stmt, 0);
        ticket->user_id = sqlite3_column_int64(stmt, 1);
        ticket->main_guild_id = sqlite3_column_int64(stmt, 2);
        ticket->staff_guild_id = sqlite3_column_int64(stmt, 3);
        ticket->staff_channel_id = sqlite3_column_int64(stmt, 4);
        ticket->dm_channel_id = sqlite3_column_int64(stmt, 5);
        ticket->status = sqlite3_column_int(stmt, 6);
        ticket->created_at = sqlite3_column_int64(stmt, 7);
        ticket->closed_at = sqlite3_column_int64(stmt, 8);
        sqlite3_finalize(stmt);
        return 0;
    }
    
    sqlite3_finalize(stmt);
    return -1;
}

int db_get_ticket_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *ticket) {
    const char *sql = "SELECT id, user_id, main_guild_id, staff_guild_id, "
                      "staff_channel_id, dm_channel_id, status, created_at, closed_at "
                      "FROM tickets WHERE (staff_channel_id = ? OR dm_channel_id = ?) "
                      "AND status = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, channel_id);
    sqlite3_bind_int64(stmt, 2, channel_id);
    sqlite3_bind_int(stmt, 3, TICKET_STATUS_OPEN);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ticket->id = sqlite3_column_int(stmt, 0);
        ticket->user_id = sqlite3_column_int64(stmt, 1);
        ticket->main_guild_id = sqlite3_column_int64(stmt, 2);
        ticket->staff_guild_id = sqlite3_column_int64(stmt, 3);
        ticket->staff_channel_id = sqlite3_column_int64(stmt, 4);
        ticket->dm_channel_id = sqlite3_column_int64(stmt, 5);
        ticket->status = sqlite3_column_int(stmt, 6);
        ticket->created_at = sqlite3_column_int64(stmt, 7);
        ticket->closed_at = sqlite3_column_int64(stmt, 8);
        sqlite3_finalize(stmt);
        return 0;
    }
    
    sqlite3_finalize(stmt);
    return -1;
}

int db_close_ticket(Database *db, int ticket_id) {
    const char *sql = "UPDATE tickets SET status = ?, closed_at = strftime('%s', 'now') "
                      "WHERE id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int(stmt, 1, TICKET_STATUS_CLOSED);
    sqlite3_bind_int(stmt, 2, ticket_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_add_ticket_message(Database *db, int ticket_id, u64_snowflake_t message_id,
                           u64_snowflake_t author_id, const char *content, 
                           const char *attachments_json, bool from_user) {
    const char *sql = "INSERT INTO ticket_messages "
                      "(ticket_id, message_id, author_id, content, attachments_json, from_user) "
                      "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int(stmt, 1, ticket_id);
    sqlite3_bind_int64(stmt, 2, message_id);
    sqlite3_bind_int64(stmt, 3, author_id);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, attachments_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, from_user ? 1 : 0);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_ticket_message(Database *db, u64_snowflake_t message_id, const char *new_content) {
    const char *sql = "UPDATE ticket_messages SET content = ?, is_edited = 1, "
                      "edited_at = strftime('%s', 'now') WHERE message_id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_text(stmt, 1, new_content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, message_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_delete_ticket_message(Database *db, u64_snowflake_t message_id) {
    const char *sql = "UPDATE ticket_messages SET is_deleted = 1 WHERE message_id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, message_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_get_ticket_messages(Database *db, int ticket_id,
                            TicketMessage **messages, int *count) {
    const char *sql = "SELECT id, ticket_id, message_id, author_id, content, attachments_json, "
                      "from_user, is_deleted, is_edited, timestamp, edited_at "
                      "FROM ticket_messages WHERE ticket_id = ? ORDER BY timestamp ASC";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int(stmt, 1, ticket_id);
    
    // Count rows
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        row_count++;
    }
    
    *count = row_count;
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        *messages = NULL;
        return 0;
    }
    
    // Allocate array
    *messages = malloc(sizeof(TicketMessage) * row_count);
    if (!*messages) {
        sqlite3_finalize(stmt);
        return -1;
    }
    
    // Reset and populate
    sqlite3_reset(stmt);
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*messages)[idx].id = sqlite3_column_int(stmt, 0);
        (*messages)[idx].ticket_id = sqlite3_column_int(stmt, 1);
        (*messages)[idx].message_id = sqlite3_column_int64(stmt, 2);
        (*messages)[idx].author_id = sqlite3_column_int64(stmt, 3);
        
        const char *content = (const char *)sqlite3_column_text(stmt, 4);
        (*messages)[idx].content = content ? strdup(content) : NULL;
        
        const char *attachments = (const char *)sqlite3_column_text(stmt, 5);
        (*messages)[idx].attachments_json = attachments ? strdup(attachments) : NULL;
        
        (*messages)[idx].from_user = sqlite3_column_int(stmt, 6) != 0;
        (*messages)[idx].is_deleted = sqlite3_column_int(stmt, 7) != 0;
        (*messages)[idx].is_edited = sqlite3_column_int(stmt, 8) != 0;
        (*messages)[idx].timestamp = sqlite3_column_int64(stmt, 9);
        (*messages)[idx].edited_at = sqlite3_column_int64(stmt, 10);
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

// HTML generation
char* generate_ticket_html(Database *db, int ticket_id, const char *username) {
    TicketMessage *messages = NULL;
    int count = 0;
    
    if (db_get_ticket_messages(db, ticket_id, &messages, &count) != 0) {
        return NULL;
    }
    
    // Allocate a large buffer for the HTML
    size_t html_size = 128 * 1024;  // 128KB initial size
    char *html = malloc(html_size);
    if (!html) {
        db_free_ticket_messages(messages, count);
        return NULL;
    }
    
    // Build HTML with staff-oriented design
    int written = snprintf(html, html_size,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>Ticket #%d - %s</title>\n"
        "    <style>\n"
        "        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
        "        body {\n"
        "            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;\n"
        "            background: #1a1d21;\n"
        "            color: #dcddde;\n"
        "            padding: 20px;\n"
        "            line-height: 1.5;\n"
        "        }\n"
        "        .container {\n"
        "            max-width: 1200px;\n"
        "            margin: 0 auto;\n"
        "            background: #2f3136;\n"
        "            border-radius: 8px;\n"
        "            overflow: hidden;\n"
        "        }\n"
        "        .header {\n"
        "            background: #202225;\n"
        "            padding: 24px 32px;\n"
        "            border-bottom: 1px solid #1a1d21;\n"
        "        }\n"
        "        .header h1 {\n"
        "            font-size: 24px;\n"
        "            font-weight: 600;\n"
        "            color: #fff;\n"
        "            margin-bottom: 8px;\n"
        "        }\n"
        "        .header .meta {\n"
        "            font-size: 14px;\n"
        "            color: #b9bbbe;\n"
        "        }\n"
        "        .header .meta span {\n"
        "            margin-right: 16px;\n"
        "        }\n"
        "        .messages {\n"
        "            padding: 24px 32px;\n"
        "        }\n"
        "        .message-group {\n"
        "            margin-bottom: 20px;\n"
        "        }\n"
        "        .message-group:hover {\n"
        "            background: #32353b;\n"
        "            margin-left: -8px;\n"
        "            margin-right: -8px;\n"
        "            padding-left: 8px;\n"
        "            padding-right: 8px;\n"
        "            border-radius: 4px;\n"
        "        }\n"
        "        .message-author {\n"
        "            display: flex;\n"
        "            align-items: baseline;\n"
        "            margin-bottom: 4px;\n"
        "        }\n"
        "        .author-name {\n"
        "            font-weight: 500;\n"
        "            font-size: 15px;\n"
        "            margin-right: 8px;\n"
        "        }\n"
        "        .message-group.user .author-name {\n"
        "            color: #5865f2;\n"
        "        }\n"
        "        .message-group.staff .author-name {\n"
        "            color: #ed4245;\n"
        "        }\n"
        "        .author-badge {\n"
        "            background: #5865f2;\n"
        "            color: #fff;\n"
        "            font-size: 10px;\n"
        "            font-weight: 600;\n"
        "            padding: 2px 4px;\n"
        "            border-radius: 3px;\n"
        "            text-transform: uppercase;\n"
        "            margin-right: 8px;\n"
        "        }\n"
        "        .message-group.staff .author-badge {\n"
        "            background: #ed4245;\n"
        "        }\n"
        "        .timestamp {\n"
        "            font-size: 12px;\n"
        "            color: #72767d;\n"
        "        }\n"
        "        .message-content {\n"
        "            color: #dcddde;\n"
        "            font-size: 15px;\n"
        "            margin-bottom: 4px;\n"
        "            white-space: pre-wrap;\n"
        "            word-wrap: break-word;\n"
        "        }\n"
        "        .message-content.deleted {\n"
        "            color: #72767d;\n"
        "            font-style: italic;\n"
        "            text-decoration: line-through;\n"
        "        }\n"
        "        .edit-indicator {\n"
        "            color: #72767d;\n"
        "            font-size: 11px;\n"
        "            margin-left: 4px;\n"
        "        }\n"
        "        .attachments {\n"
        "            margin-top: 8px;\n"
        "            padding: 8px;\n"
        "            background: #202225;\n"
        "            border-radius: 4px;\n"
        "            border-left: 2px solid #5865f2;\n"
        "        }\n"
        "        .attachment-item {\n"
        "            color: #00a8fc;\n"
        "            text-decoration: none;\n"
        "            font-size: 14px;\n"
        "            display: block;\n"
        "            margin: 4px 0;\n"
        "        }\n"
        "        .attachment-item:hover {\n"
        "            text-decoration: underline;\n"
        "        }\n"
        "        .attachment-item::before {\n"
        "            content: '📎 ';\n"
        "        }\n"
        "        .footer {\n"
        "            background: #202225;\n"
        "            padding: 16px 32px;\n"
        "            border-top: 1px solid #1a1d21;\n"
        "            text-align: center;\n"
        "            color: #72767d;\n"
        "            font-size: 12px;\n"
        "        }\n"
        "        .internal-note {\n"
        "            background: #faa81a;\n"
        "            color: #000;\n"
        "            padding: 12px;\n"
        "            border-radius: 4px;\n"
        "            margin-bottom: 16px;\n"
        "            font-size: 14px;\n"
        "            font-weight: 500;\n"
        "        }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <div class=\"header\">\n"
        "            <h1>Ticket #%d</h1>\n"
        "            <div class=\"meta\">\n"
        "                <span><strong>User:</strong> %s</span>\n"
        "                <span><strong>Messages:</strong> %d</span>\n"
        "                <span><strong>Status:</strong> Closed</span>\n"
        "            </div>\n"
        "        </div>\n"
        "        <div class=\"messages\">\n"
        "            <div class=\"internal-note\">⚠️ STAFF ONLY - Internal Ticket Transcript</div>\n",
        ticket_id, username, ticket_id, username, count);
    
    // Group consecutive messages from the same author
    for (int i = 0; i < count; i++) {
        bool is_user = messages[i].from_user;
        
        // Check if this is the start of a new group
        bool start_group = (i == 0) || (messages[i-1].from_user != is_user) || 
                          (!is_user && messages[i-1].author_id != messages[i].author_id);
        
        if (start_group) {
            // Start a new message group
            time_t first_ts = (time_t)messages[i].timestamp;
            struct tm *first_tm = gmtime(&first_ts);
            char time_buf[64];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", first_tm);
            
            // Get author name
            char author_name[128];
            if (is_user) {
                snprintf(author_name, sizeof(author_name), "%s", username);
            } else {
                // Fetch staff member's username
                struct discord_user staff_info = { 0 };
                discord_get_user(g_client, messages[i].author_id, &staff_info);
                if (staff_info.username && staff_info.username[0] != '\0') {
                    snprintf(author_name, sizeof(author_name), "%s", staff_info.username);
                    discord_user_cleanup(&staff_info);
                } else {
                    snprintf(author_name, sizeof(author_name), "Staff Member (ID: %" PRIu64 ")", messages[i].author_id);
                }
            }
            
            char group_header[512];
            snprintf(group_header, sizeof(group_header),
                "            <div class=\"message-group %s\">\n"
                "                <div class=\"message-author\">\n"
                "                    <span class=\"author-badge\">%s</span>\n"
                "                    <span class=\"author-name\">%s</span>\n"
                "                    <span class=\"timestamp\">%s</span>\n"
                "                </div>\n",
                is_user ? "user" : "staff",
                is_user ? "USER" : "STAFF",
                author_name,
                time_buf);
            
            strncat(html, group_header, html_size - strlen(html) - 1);
        }
        
        // Escape HTML in content
        char *escaped_content = malloc(strlen(messages[i].content) * 6 + 1);
        const char *src = messages[i].content;
        char *dst = escaped_content;
        while (*src) {
            switch (*src) {
                case '<': strcpy(dst, "&lt;"); dst += 4; break;
                case '>': strcpy(dst, "&gt;"); dst += 4; break;
                case '&': strcpy(dst, "&amp;"); dst += 5; break;
                case '"': strcpy(dst, "&quot;"); dst += 6; break;
                case '\'': strcpy(dst, "&#39;"); dst += 5; break;
                default: *dst++ = *src; break;
            }
            src++;
        }
        *dst = '\0';
        
        // Add message content with edit/delete indicators
        char content_html[4096];
        if (messages[i].is_deleted) {
            snprintf(content_html, sizeof(content_html),
                "                <div class=\"message-content deleted\">[Message Deleted]</div>\n");
        } else {
            snprintf(content_html, sizeof(content_html),
                "                <div class=\"message-content\">%s",
                escaped_content);
            
            if (messages[i].is_edited) {
                strncat(content_html, "<span class=\"edit-indicator\">(edited)</span>", 
                       sizeof(content_html) - strlen(content_html) - 1);
            }
            
            strncat(content_html, "</div>\n", sizeof(content_html) - strlen(content_html) - 1);
        }
        
        free(escaped_content);
        strncat(html, content_html, html_size - strlen(html) - 1);
        
        // Add attachments if present
        if (messages[i].attachments_json && strlen(messages[i].attachments_json) > 2) {
            strncat(html, "                <div class=\"attachments\">\n", 
                   html_size - strlen(html) - 1);
            
            // Parse simple JSON array (just extract URLs between quotes)
            const char *json = messages[i].attachments_json;
            const char *url_start = strchr(json, '"');
            while (url_start) {
                url_start++; // Move past opening quote
                const char *url_end = strchr(url_start, '"');
                if (!url_end) break;
                
                size_t url_len = url_end - url_start;
                char url[512];
                if (url_len < sizeof(url)) {
                    strncpy(url, url_start, url_len);
                    url[url_len] = '\0';
                    
                    char att_html[600];
                    snprintf(att_html, sizeof(att_html),
                            "                    <a href=\"%s\" class=\"attachment-item\" target=\"_blank\">%s</a>\n",
                            url, url);
                    strncat(html, att_html, html_size - strlen(html) - 1);
                }
                
                url_start = strchr(url_end + 1, '"');
            }
            
            strncat(html, "                </div>\n", html_size - strlen(html) - 1);
        }
        
        // Check if we should close the group
        bool end_group = (i == count - 1) || (messages[i+1].from_user != is_user) ||
                        (!is_user && i + 1 < count && messages[i+1].author_id != messages[i].author_id);
        if (end_group) {
            strncat(html, "            </div>\n", html_size - strlen(html) - 1);
        }
    }
    
    // Close HTML
    const char *footer = 
        "        </div>\n"
        "        <div class=\"footer\">\n"
        "            Internal Staff Transcript | Confidential\n"
        "        </div>\n"
        "    </div>\n"
        "</body>\n"
        "</html>\n";
    
    strncat(html, footer, html_size - strlen(html) - 1);
    
    db_free_ticket_messages(messages, count);
    return html;
}

// Command handlers
static void handle_ticket_create(struct discord *client,
                                  const struct discord_interaction *event) {
    // This command should only work in DMs
    if (event->guild_id != 0) {
        struct discord_interaction_response response = {
            .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
            .data = &(struct discord_interaction_callback_data){
                .content = "❌ This command can only be used in DMs with the bot!",
                .flags = 1 << 6,  // Ephemeral
            },
        };
        discord_create_interaction_response(client, event->id, event->token, &response, NULL);
        return;
    }
    
    // Check if user already has an open ticket
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
    
    // Get the server ID from options
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
    
    // Get server configuration
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
    
    // Create DM channel
    struct discord_create_dm_params dm_params = {
        .recipient_id = event->user->id
    };
    struct discord_channel dm_channel = { 0 };
    
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
    
    // Create ticket channel in staff server
    char channel_name[64];
    snprintf(channel_name, sizeof(channel_name), "ticket-%s", event->user->username);
    
    struct discord_create_guild_channel_params channel_params = {
        .name = channel_name,
        .type = DISCORD_CHANNEL_GUILD_TEXT,
        .parent_id = config.ticket_category_id,
    };
    
    struct discord_channel staff_channel = { 0 };
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
    
    // Create ticket in database
    int ticket_id = db_create_ticket(g_db, event->user->id, main_guild_id,
                                      config.staff_guild_id, staff_channel.id,
                                      dm_channel.id);
    
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
    
    // Send initial messages
    char staff_msg[512];
    snprintf(staff_msg, sizeof(staff_msg),
             "🎫 **New Ticket #%d**\n\n"
             "**User:** %s (ID: %" PRIu64 ")\n"
             "**Server:** %" PRIu64 "\n\n"
             "All messages in this channel will be relayed to the user anonymously.\n"
             "Use `/closeticket` to close this ticket.",
             ticket_id, event->user->username, event->user->id, main_guild_id);
    
    struct discord_create_message_params staff_params = {
        .content = staff_msg
    };
    discord_create_message(client, staff_channel.id, &staff_params, NULL);
    
    char user_msg[256];
    snprintf(user_msg, sizeof(user_msg),
             "✅ **Ticket Created!**\n\n"
             "Your ticket #%d has been created. Staff members will respond to you here.\n"
             "All messages you send here will be forwarded to staff anonymously.",
             ticket_id);
    
    struct discord_create_message_params user_params = {
        .content = user_msg
    };
    discord_create_message(client, dm_channel.id, &user_params, NULL);
    
    // Respond to interaction
    char response_text[128];
    snprintf(response_text, sizeof(response_text),
             "✅ Ticket #%d created! Check your DMs to continue.",
             ticket_id);
    
    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = response_text,
            .flags = 1 << 6,
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &response, NULL);
    
    discord_channel_cleanup(&dm_channel);
    discord_channel_cleanup(&staff_channel);
}

static void handle_ticket_close(struct discord *client,
                                 const struct discord_interaction *event) {
    // Get ticket by channel
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
    
    // Close ticket in database
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
    
    // Generate HTML log
    struct discord_user user_info = { 0 };
    discord_get_user(client, ticket.user_id, &user_info);
    const char *username = user_info.username ? user_info.username : "Unknown User";
    
    char *html = generate_ticket_html(g_db, ticket.id, username);
    
    if (html) {
        // Save HTML to file
        char filename[128];
        snprintf(filename, sizeof(filename), "ticket_%d.html", ticket.id);
        
        FILE *f = fopen(filename, "w");
        if (f) {
            fprintf(f, "%s", html);
            fclose(f);
            
            // Get server config for log channel
            ServerConfig config;
            if (db_get_server_config(g_db, ticket.main_guild_id, &config) == 0) {
                // Send log file to log channel with file attachment
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg),
                         "📋 **Ticket #%d Closed**\n"
                         "**Closed by:** <@%" PRIu64 ">\n"
                         "**User:** %s (%" PRIu64 ")\n"
                         "**Transcript:** See attached HTML file",
                         ticket.id,
                         event->member ? event->member->user->id : 0,
                         username,
                         ticket.user_id);
                
                struct discord_attachment attachment = {
                    .filename = filename,
                };
                
                struct discord_attachment *attachments[] = {
                    &attachment,
                    NULL
                };
                
                struct discord_create_message_params log_params = {
                    .content = log_msg,
                    .attachments = attachments
                };
                
                discord_create_message(client, config.log_channel_id, &log_params, NULL);
            }
            
            // Clean up file after sending
            remove(filename);
        }
        free(html);
    }
    
    // Notify user
    char user_msg[128];
    snprintf(user_msg, sizeof(user_msg),
             "🔒 Your ticket #%d has been closed by staff.\nThank you for contacting us!",
             ticket.id);
    
    struct discord_create_message_params user_params = {
        .content = user_msg
    };
    discord_create_message(client, ticket.dm_channel_id, &user_params, NULL);
    
    // Respond to interaction
    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = "✅ Ticket closed! This channel will be deleted in 10 seconds.",
        },
    };
    discord_create_interaction_response(client, event->id, event->token, &response, NULL);
    
    // Delete channel after delay (would need timer implementation)
    // For now, just delete immediately
    discord_delete_channel(client, event->channel_id, NULL);
    
    discord_user_cleanup(&user_info);
}

static void handle_config_set(struct discord *client,
                               const struct discord_interaction *event) {
    // Get options
    const char *main_guild_str = NULL;
    const char *staff_guild_str = NULL;
    const char *category_str = NULL;
    const char *log_channel_str = NULL;
    
    if (event->data && event->data->options) {
        for (int i = 0; event->data->options[i]; i++) {
            const char *name = event->data->options[i]->name;
            const char *value = event->data->options[i]->value;
            
            if (strcmp(name, "main_server") == 0) main_guild_str = value;
            else if (strcmp(name, "staff_server") == 0) staff_guild_str = value;
            else if (strcmp(name, "category") == 0) category_str = value;
            else if (strcmp(name, "log_channel") == 0) log_channel_str = value;
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
    
    u64_snowflake_t main_guild = (u64_snowflake_t)strtoull(main_guild_str, NULL, 10);
    u64_snowflake_t staff_guild = (u64_snowflake_t)strtoull(staff_guild_str, NULL, 10);
    u64_snowflake_t category = (u64_snowflake_t)strtoull(category_str, NULL, 10);
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

// Message handler for ticket communication
void on_ticket_message(struct discord *client,
                       const struct discord_message *event) {
    // Ignore bot messages
    if (event->author->bot) return;
    
    // Ignore messages that match our forwarding pattern to prevent duplicates
    if (event->content && strncmp(event->content, "**", 2) == 0) {
        const char *colon_pos = strstr(event->content, ":** ");
        if (colon_pos != NULL) {
            // Extract the prefix
            size_t prefix_len = colon_pos - event->content;
            char prefix[256];
            if (prefix_len < sizeof(prefix) - 1) {
                strncpy(prefix, event->content, prefix_len);
                prefix[prefix_len] = '\0';
                
                // Check if it's "**Staff:**" or "**username:**"
                if (strcmp(prefix, "**Staff") == 0 || strstr(prefix, "**") == prefix) {
                    return;  // This is already a forwarded message
                }
            }
        }
    }
    
    // Check if this is a ticket channel
    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, event->channel_id, &ticket) != 0) {
        return;  // Not a ticket channel
    }
    
    bool from_user = (event->channel_id == ticket.dm_channel_id);
    
    // Build attachments JSON if any
    char *attachments_json = NULL;
    // Check if attachments exist and the first one is not NULL
    if (event->attachments && event->attachments[0]) {
        size_t json_size = 1024;
        attachments_json = malloc(json_size);
        strcpy(attachments_json, "[");
        
        // Iterate until we hit a NULL pointer
        for (int i = 0; event->attachments[i]; i++) {
            struct discord_attachment *att = event->attachments[i];
            
            if (att->url) {
                char entry[512];
                // Add comma if not the first item
                snprintf(entry, sizeof(entry), "%s\"%s\"", i > 0 ? "," : "", att->url);
                strncat(attachments_json, entry, json_size - strlen(attachments_json) - 1);
            }
        }
        strncat(attachments_json, "]", json_size - strlen(attachments_json) - 1);
    }
    
    // Log message with message ID and attachments
    db_add_ticket_message(g_db, ticket.id, event->id, event->author->id, 
                         event->content ? event->content : "", 
                         attachments_json, from_user);
    
    free(attachments_json);
    
    // Forward message
    u64_snowflake_t target_channel = from_user ? ticket.staff_channel_id : ticket.dm_channel_id;
    
    // Build message content
    char forwarded[2048];
    const char *content = event->content ? event->content : "";
    if (from_user) {
        // User to staff - show username
        snprintf(forwarded, sizeof(forwarded), "**%s:** %s", event->author->username, content);
    } else {
        // Staff to user - anonymous
        snprintf(forwarded, sizeof(forwarded), "**Staff:** %s", content);
    }
    
    // Add attachment info
    if (event->attachments && event->attachments[0]) {
        strncat(forwarded, "\n📎 Attachments:", sizeof(forwarded) - strlen(forwarded) - 1);
        
        // Iterate until we hit a NULL pointer
        for (int i = 0; event->attachments[i]; i++) {
            struct discord_attachment *att = event->attachments[i];
            
            if (att->url) {
                char att_line[256];
                snprintf(att_line, sizeof(att_line), "\n• %s", att->url);
                strncat(forwarded, att_line, sizeof(forwarded) - strlen(forwarded) - 1);
            }
        }
    }
    
    struct discord_create_message_params params = {
        .content = forwarded
    };
    
    discord_create_message(client, target_channel, &params, NULL);
}

// Message update handler
void on_ticket_message_update(struct discord *client,
                               const struct discord_message *event) {
    // Ignore bot messages
    if (event->author && event->author->bot) return;
    
    // Check if this is a ticket message
    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, event->channel_id, &ticket) != 0) {
        return;  // Not a ticket channel
    }
    
    // Update the message in database
    if (event->content) {
        db_update_ticket_message(g_db, event->id, event->content);
    }
    
    // Forward edit notification
    bool from_user = (event->channel_id == ticket.dm_channel_id);
    u64_snowflake_t target_channel = from_user ? ticket.staff_channel_id : ticket.dm_channel_id;
    
    char edit_notice[2048];
    if (from_user) {
        snprintf(edit_notice, sizeof(edit_notice), 
                 "✏️ **%s** edited their message:\n**New content:** %s", 
                 event->author ? event->author->username : "User",
                 event->content ? event->content : "(no content)");
    } else {
        snprintf(edit_notice, sizeof(edit_notice), 
                 "✏️ **Staff** edited their message:\n**New content:** %s",
                 event->content ? event->content : "(no content)");
    }
    
    struct discord_create_message_params params = {
        .content = edit_notice
    };
    
    discord_create_message(client, target_channel, &params, NULL);
}

// Message delete handler
void on_ticket_message_delete(struct discord *client,
                               u64_snowflake_t message_id,
                               u64_snowflake_t channel_id) {
    // Check if this is a ticket channel
    Ticket ticket;
    if (db_get_ticket_by_channel(g_db, channel_id, &ticket) != 0) {
        return;  // Not a ticket channel
    }
    
    // Mark message as deleted in database
    db_delete_ticket_message(g_db, message_id);
    
    // Forward delete notification
    bool from_user = (channel_id == ticket.dm_channel_id);
    u64_snowflake_t target_channel = from_user ? ticket.staff_channel_id : ticket.dm_channel_id;
    
    char delete_notice[256];
    if (from_user) {
        snprintf(delete_notice, sizeof(delete_notice), 
                 "🗑️ User deleted a message");
    } else {
        snprintf(delete_notice, sizeof(delete_notice), 
                 "🗑️ Staff deleted a message");
    }
    
    struct discord_create_message_params params = {
        .content = delete_notice
    };
    
    discord_create_message(client, target_channel, &params, NULL);
}

// Interaction router
void on_ticket_interaction(struct discord *client,
                           const struct discord_interaction *event) {
    if (!event->data) return;
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;
    
    const char *cmd = event->data->name;
    
    if (strcmp(cmd, "ticket") == 0) {
        handle_ticket_create(client, event);
    } else if (strcmp(cmd, "closeticket") == 0) {
        handle_ticket_close(client, event);
    } else if (strcmp(cmd, "ticketconfig") == 0) {
        handle_config_set(client, event);
    }
}

// Register commands
void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id) {
    // /ticket command (global, for DMs)
    struct discord_application_command_option server_opt = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "server",
        .description = "The server ID you want to contact staff for",
        .required = true,
    };
    
    struct discord_application_command_option *ticket_opts[] = {
        &server_opt, NULL
    };
    
    struct discord_create_global_application_command_params ticket_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "ticket",
        .description = "Create a support ticket (use in DMs)",
        .options = ticket_opts,
    };
    
    discord_create_global_application_command(client, application_id, &ticket_params, NULL);
    
    // /closeticket command (guild)
    struct discord_create_guild_application_command_params close_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "closeticket",
        .description = "Close the current ticket",
    };
    
    discord_create_guild_application_command(client, application_id, guild_id, &close_params, NULL);
    
    // /ticketconfig command (guild)
    struct discord_application_command_option main_server = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "main_server",
        .description = "Main server ID (where users are)",
        .required = true,
    };
    
    struct discord_application_command_option staff_server = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "staff_server",
        .description = "Staff server ID (where tickets are created)",
        .required = true,
    };
    
    struct discord_application_command_option category = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "category",
        .description = "Category ID for ticket channels",
        .required = true,
    };
    
    struct discord_application_command_option log_channel = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "log_channel",
        .description = "Channel ID for ticket logs",
        .required = true,
    };
    
    struct discord_application_command_option *config_opts[] = {
        &main_server, &staff_server, &category, &log_channel, NULL
    };
    
    struct discord_create_guild_application_command_params config_params = {
        .type = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name = "ticketconfig",
        .description = "Configure ticket system (Admin only)",
        .options = config_opts,
    };
    
    discord_create_guild_application_command(client, application_id, guild_id, &config_params, NULL);
    
    printf("[ticket] Ticket commands registered\n");
}

// Module initialisation
void ticket_module_init(struct discord *client, Database *db) {
    g_db = db;
    g_client = client;
    
    // Initialise ticket tables
    init_ticket_tables(db);
    
    printf("[ticket] Ticket module initialised\n");
}