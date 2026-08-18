#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_string(const char *s)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s;

    (void)putchar('"');
    while (*p != '\0') {
        switch (*p) {
        case '"': (void)fputs("\\\"", stdout); break;
        case '\\': (void)fputs("\\\\", stdout); break;
        case '\b': (void)fputs("\\b", stdout); break;
        case '\f': (void)fputs("\\f", stdout); break;
        case '\n': (void)fputs("\\n", stdout); break;
        case '\r': (void)fputs("\\r", stdout); break;
        case '\t': (void)fputs("\\t", stdout); break;
        default:
            if (*p < 0x20u) {
                (void)fprintf(stdout, "\\u00%c%c", hex[*p >> 4u],
                              hex[*p & 0x0fu]);
            } else {
                (void)putchar((int)*p);
            }
            break;
        }
        p++;
    }
    (void)putchar('"');
}

static void stable_sort(char **items, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        char *item = items[i];
        size_t j = i;

        while (j > 0u && strcmp(items[j - 1u], item) > 0) {
            items[j] = items[j - 1u];
            j--;
        }
        items[j] = item;
    }
}

int main(int argc, char **argv)
{
    char **sources;
    size_t nsource;
    size_t i;
    int sep;
    int arg;

    for (sep = 2; sep < argc && strcmp(argv[sep], "--") != 0; sep++) {}
    if (argc < 5 || sep == 2 || sep == argc - 1 || sep == argc) {
        (void)fprintf(stderr,
                      "usage: %s DIRECTORY COMPILER [FLAGS ...] -- SOURCE...\n",
                      argv[0]);
        return 2;
    }

    nsource = (size_t)(argc - sep - 1);
    sources = malloc(nsource * sizeof(*sources));
    if (sources == NULL) {
        (void)fputs("gen-compdb: out of memory\n", stderr);
        return 1;
    }
    for (i = 0u; i < nsource; i++)
        sources[i] = argv[sep + 1 + (int)i];
    stable_sort(sources, nsource);

    (void)putchar('[');
    for (i = 0u; i < nsource; i++) {
        if (i != 0u)
            (void)putchar(',');
        (void)fputs("{\"directory\":", stdout);
        json_string(argv[1]);
        (void)fputs(",\"file\":", stdout);
        json_string(sources[i]);
        (void)fputs(",\"arguments\":[", stdout);
        for (arg = 2; arg < sep; arg++) {
            if (arg != 2)
                (void)putchar(',');
            json_string(argv[arg]);
        }
        (void)fputs(",\"-c\",", stdout);
        json_string(sources[i]);
        (void)fputs("]}", stdout);
    }
    (void)fputs("]\n", stdout);
    free(sources);

    if (ferror(stdout)) {
        (void)fputs("gen-compdb: write failed\n", stderr);
        return 1;
    }
    return 0;
}
