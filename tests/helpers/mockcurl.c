#define YEW_MOCKAI_NO_MAIN
#include "mockai.c"

#include <fcntl.h>

static bool mockcurl_read_config(unsigned char **out, size_t *outlen)
{
    unsigned char *config = malloc(MOCK_MAX_REQUEST + 1U);
    size_t len = 0U;

    if (config == NULL) {
        mock_system_error("malloc");
        return false;
    }
    while (len < MOCK_MAX_REQUEST) {
        ssize_t n = read(STDIN_FILENO, config + len, MOCK_MAX_REQUEST - len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            mock_system_error("read curl config");
            free(config);
            return false;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    if (len == MOCK_MAX_REQUEST) {
        mock_error("curl config exceeds 1 MiB");
        free(config);
        return false;
    }
    config[len] = '\0';
    *out = config;
    *outlen = len;
    return true;
}

static bool mockcurl_has(const unsigned char *config, const char *text)
{
    return strstr((const char *)config, text) != NULL;
}

static bool mockcurl_has_ci(const unsigned char *config, const char *text)
{
    size_t have = strlen((const char *)config);
    size_t want = strlen(text);
    size_t at;

    for (at = 0U; at + want <= have; at++) {
        size_t i;

        for (i = 0U; i < want; i++) {
            unsigned char a = config[at + i];
            unsigned char b = (unsigned char)text[i];

            if (a >= (unsigned char)'A' && a <= (unsigned char)'Z')
                a = (unsigned char)(a - (unsigned char)'A' + 'a');
            if (b >= (unsigned char)'A' && b <= (unsigned char)'Z')
                b = (unsigned char)(b - (unsigned char)'A' + 'a');
            if (a != b)
                break;
        }
        if (i == want)
            return true;
    }
    return false;
}

static bool mockcurl_config_valid(const unsigned char *config)
{
    bool auth = mockcurl_has_ci(config,
                                "header = \"authorization: bearer ") ||
                mockcurl_has_ci(config, "header = \"x-api-key: ");

    if (!mockcurl_has(config, "url = \"") ||
        !mockcurl_has(config, "request = \"POST\"") ||
        !mockcurl_has(config, "data-binary = \"") ||
        !mockcurl_has_ci(config,
                         "header = \"content-type: application/json\"") ||
        !mockcurl_has(config, "write-out = \"%{stderr}yew-http-status: ") ||
        !auth) {
        mock_error("curl config is missing a required directive or header");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *script_path;
    unsigned char *config;
    size_t config_len;
    MockScript script;
    char status[64];
    int n;

    if (argc == 2 && strcmp(argv[1], "--version") == 0)
        return mock_write_all(STDOUT_FILENO, "curl 8.5.0 mockcurl\n", 20U,
                              false) ? 0 : 2;
    if (argc != 5 || strcmp(argv[1], "-sS") != 0 ||
        strcmp(argv[2], "--no-buffer") != 0 ||
        strcmp(argv[3], "--config") != 0 || strcmp(argv[4], "-") != 0) {
        mock_error("expected: mockcurl -sS --no-buffer --config -");
        return 2;
    }
    script_path = getenv("YEW_AI_MOCK_SCRIPT");
    if (script_path == NULL || *script_path == '\0') {
        mock_error("YEW_AI_MOCK_SCRIPT is required");
        return 2;
    }
    if (!mockcurl_read_config(&config, &config_len))
        return 2;
    (void)config_len;
    if (!mockcurl_config_valid(config)) {
        free(config);
        return 2;
    }
    free(config);
    if (!mock_script_load(script_path, &script))
        return 2;
    (void)signal(SIGPIPE, SIG_IGN);
    if (!mock_script_replay(&script, STDOUT_FILENO, false)) {
        mock_script_free(&script);
        return 2;
    }
    n = snprintf(status, sizeof(status), "yew-http-status: %u\n",
                 script.status);
    if (n < 0 || (size_t)n >= sizeof(status) ||
        !mock_write_all(STDERR_FILENO, status, (size_t)n, false)) {
        mock_script_free(&script);
        return 2;
    }
    n = script.exit_code;
    mock_script_free(&script);
    return n;
}
