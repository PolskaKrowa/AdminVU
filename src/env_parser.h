#ifndef ENV_PARSER_H
#define ENV_PARSER_H

// Load environment variables from a .env file
// Returns 0 on success, -1 on failure
int load_env_file(const char *filepath);

// Get the value of an environment variable
// First checks actual environment, then loaded .env file
const char *get_env(const char *key);

// Free resources used by env parser
void cleanup_env(void);

#endif // ENV_PARSER_H
