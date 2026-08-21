#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/ai/ai.h"
#include "mod/ai/redact.h"
#include "util/base.h"

typedef struct PathCase {
    const char *path;
    const char *pattern;
} PathCase;

static const PathCase default_cases[] = {
    {".env", ".env*"}, {"config/.ENV.production", ".env*"},
    {"src/client-secret.txt", "*secret*"},
    {"vendor/SECRET/data.c", "*secret*"},
    {"aws-credentials.json", "*credential*"},
    {"vendor/.ssh/id_rsa", ".ssh/"}, {"certs/server.PEM", "*.pem"},
    {"private.key", "*.key"}, {"cert/client.p12", "*.p12"},
    {"cert/client.PFX", "*.pfx"}, {"java/store.jks", "*.jks"},
    {"java/prod.keystore", "*.keystore"}, {"keys/id_rsa_old", "id_rsa*"},
    {"keys/id_ECDSA", "id_ecdsa*"}, {"id_ed25519.backup", "id_ed25519*"},
    {"home/.netrc", ".netrc"}, {"_NETRC", "_netrc"},
    {"web/.npmrc", ".npmrc"}, {"python/.PYPIRC", ".pypirc"},
    {"vendor/.aws/config", ".aws/"}, {".GNUPG/private-keys-v1.d/key", ".gnupg/"},
    {".Docker/Config.Json", ".docker/config.json"},
    {"vault/passwords.kdbx", "*.kdbx"}, {"archive/data.GPG", "*.gpg"},
    {"keys/public.asc", "*.asc"}
};

void test_ai_paths_all_defaults_case_and_nesting(void)
{
    AiPathPolicy *policy = yew_ai_path_policy_new(NULL, 0U, false, NULL);
    char absolute[4096];
    Ed ed;
    size_t i;

    YEW_ASSERT_NOT_NULL(policy);
    YEW_ASSERT_EQ_U64(yew_ai_path_policy_len(policy), 23U);
    for (i = 0U; i < YEW_ARRAY_LEN(default_cases); i++) {
        AiPathHit hit;

        if (!yew_ai_path_excluded(policy, default_cases[i].path, &hit))
            (void)fprintf(stderr, "path fixture missed %s\n",
                          default_cases[i].path);
        YEW_ASSERT(yew_ai_path_excluded(policy, default_cases[i].path, &hit));
        YEW_ASSERT_EQ_STR(hit.pattern, default_cases[i].pattern);
    }
    yew_ed_init(&ed);
    YEW_ASSERT(snprintf(absolute, sizeof(absolute), "%s/%s",
                        yew_ws_root(&ed), ".docker/config.json") > 0);
    YEW_ASSERT_EQ_STR(yew_ai_path_exclusion(&ed, absolute),
                      ".docker/config.json");
    YEW_ASSERT_EQ_STR(yew_ai_path_exclusion(&ed,
                      "./vendor/.ssh/id_ed25519"), ".ssh/");
    YEW_ASSERT_NULL(yew_ai_path_exclusion(&ed, "src/editor.c"));
    yew_ed_free(&ed);
    yew_ai_path_policy_free(policy);
}

void test_ai_paths_nonmatches_and_separator_boundary(void)
{
    static const char *const safe[] = {
        "src/environment.c", "docs/public.pem.txt", "ssh/rsa_id", "src/key.c",
        "identity-safe/readme.txt", "cert/no-extension", "docker/config.json",
        "src/sec/ret.c", "notes/envoy", "a/.sshing/file"
    };
    AiPathPolicy *policy = yew_ai_path_policy_new(NULL, 0U, false, NULL);
    size_t i;

    YEW_ASSERT_NOT_NULL(policy);
    for (i = 0U; i < YEW_ARRAY_LEN(safe); i++) {
        if (yew_ai_path_excluded(policy, safe[i], NULL))
            (void)fprintf(stderr, "safe path excluded %s\n", safe[i]);
        YEW_ASSERT(!yew_ai_path_excluded(policy, safe[i], NULL));
    }
    /* A slash always separates components; '*' and '?' never consume it. */
    YEW_ASSERT(!yew_ai_path_glob_valid("foo/**/bar"));
    yew_ai_path_policy_free(policy);
}

void test_ai_paths_user_merge_replace_question_and_star(void)
{
    static const char *const user[] = {"wolf-?.token", "generated/*", "bad/**"};
    AiPathError err;
    AiPathHit hit;
    AiPathPolicy *merged = yew_ai_path_policy_new(user, 3U, false, &err);
    AiPathPolicy *replaced = yew_ai_path_policy_new(user, 2U, true, NULL);

    YEW_ASSERT_NOT_NULL(merged);
    YEW_ASSERT_EQ_U64(yew_ai_path_policy_len(merged), 25U);
    YEW_ASSERT_EQ_STR(err.pattern, "bad/**");
    YEW_ASSERT_EQ_U64(err.index, 2U);
    YEW_ASSERT_NOT_NULL(err.message);
    YEW_ASSERT(yew_ai_path_excluded(merged, "src/wolf-a.token", &hit));
    YEW_ASSERT_EQ_STR(hit.pattern, "wolf-?.token");
    YEW_ASSERT(!yew_ai_path_excluded(merged, "src/wolf-ab.token", NULL));
    YEW_ASSERT(yew_ai_path_excluded(merged, ".env.local", NULL));

    YEW_ASSERT_NOT_NULL(replaced);
    YEW_ASSERT_EQ_U64(yew_ai_path_policy_len(replaced), 2U);
    YEW_ASSERT(!yew_ai_path_excluded(replaced, ".env.local", NULL));
    YEW_ASSERT(yew_ai_path_excluded(replaced, "generated/item", NULL));
    YEW_ASSERT(!yew_ai_path_excluded(replaced, "generated/nested/item", NULL));
    yew_ai_path_policy_free(merged);
    yew_ai_path_policy_free(replaced);
}

void test_ai_paths_null_and_empty_lifecycle(void)
{
    AiPathPolicy *empty = yew_ai_path_policy_new(NULL, 0U, true, NULL);

    YEW_ASSERT_NOT_NULL(empty);
    YEW_ASSERT_EQ_U64(yew_ai_path_policy_len(empty), 0U);
    YEW_ASSERT(!yew_ai_path_excluded(empty, "anything", NULL));
    YEW_ASSERT(!yew_ai_path_excluded(NULL, "anything", NULL));
    YEW_ASSERT(!yew_ai_path_excluded(empty, NULL, NULL));
    YEW_ASSERT(!yew_ai_path_glob_valid(NULL));
    YEW_ASSERT(!yew_ai_path_glob_valid(""));
    YEW_ASSERT(yew_ai_path_glob_valid("*.key"));
    yew_ai_path_policy_free(empty);
    yew_ai_path_policy_free(NULL);
}
