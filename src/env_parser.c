#include "env_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ENV_VARS 100
#define MAX_LINE_LENGTH 1024

typedef struct {
    char *key;
    char *value;
} EnvVar;

static EnvVar env_vars[MAX_ENV_VARS];
static int env_count = 0;

// Trim whitespace from both ends of a string
static char *trim_whitespace(char *str) {
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return str;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    // Write new null terminator
    *(end + 1) = '\0';

    return str;
}

// Parse a line from .env file
static int parse_env_line(char *line) {
    // Skip comments and empty lines
    char *trimmed = trim_whitespace(line);
    if (trimmed[0] == '#' || trimmed[0] == '\0') {
        return 0;
    }

    // Find the '=' separator
    char *equals = strchr(trimmed, '=');
    if (!equals) {
        return 0; // Invalid line, skip
    }

    // Split into key and value
    *equals = '\0';
    char *key = trim_whitespace(trimmed);
    char *value = trim_whitespace(equals + 1);

    // Remove quotes from value if present
    if ((value[0] == '"' || value[0] == '\'') && 
        value[strlen(value) - 1] == value[0]) {
        value[strlen(value) - 1] = '\0';
        value++;
    }

    if (env_count >= MAX_ENV_VARS) {
        fprintf(stderr, "Warning: Maximum number of environment variables reached\n");
        return -1;
    }

    // Store the key-value pair
    env_vars[env_count].key = strdup(key);
    env_vars[env_count].value = strdup(value);
    
    if (!env_vars[env_count].key || !env_vars[env_count].value) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return -1;
    }

    env_count++;
    return 0;
}

int load_env_file(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        return -1;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\r\n")] = '\0';
        parse_env_line(line);
    }

    fclose(file);
    return 0;
}

const char *get_env(const char *key) {
    // First check actual environment variables
    const char *env_val = getenv(key);
    if (env_val) {
        return env_val;
    }

    // Then check loaded .env file
    for (int i = 0; i < env_count; i++) {
        if (strcmp(env_vars[i].key, key) == 0) {
            return env_vars[i].value;
        }
    }

    return NULL;
}

void cleanup_env(void) {
    for (int i = 0; i < env_count; i++) {
        free(env_vars[i].key);
        free(env_vars[i].value);
    }
    env_count = 0;
}
