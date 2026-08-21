#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mod/ai/policy.h"
#include "util/base.h"

static char *temp_policy(const char *text)
{
    char pattern[] = "/tmp/yew-ai-policy-XXXXXX";
    int fd = mkstemp(pattern);
    size_t len = strlen(text);
    size_t off = 0U;
    char *path;

    YEW_ASSERT(fd >= 0);
    while (off < len) {
        ssize_t wrote = write(fd, text + off, len - off);

        YEW_ASSERT(wrote > 0);
        off += (size_t)wrote;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
    path = yew_xmalloc(strlen(pattern) + 1U);
    (void)memcpy(path, pattern, strlen(pattern) + 1U);
    return path;
}

static bool policy_scan(const AiRedactPolicy *policy, const char *text,
                        const char *rule)
{
    AiCtx context;
    RedactHit hit;

    (void)memset(&context, 0, sizeof(context));
    context.prefix = (const u8 *)text;
    context.plen = (u32)strlen(text);
    context.line_1based = 1U;
    return yew_ai_redact_scan(policy, &context, &hit) &&
           strcmp(hit.rule, rule) == 0;
}

void test_ai_policy_user_rows_append_and_invalid_rows_are_isolated(void)
{
    const char *shipped_text =
        "[\n"
        " { name: \"ship\", re: \"SHIP[0-9]+\", flags: \"\", note: \"shipped\" },\n"
        "]\n";
    const char *user_text =
        "[\n"
        " { name: \"bad-shape\", re: 3, flags: \"\", note: \"bad\" },\n"
        " { name: \"bad-regex\", re: \"(\", flags: \"\", note: \"bad\" },\n"
        " { name: \"user\", re: \"USER[0-9]+\", flags: \"i\", note: \"user\" },\n"
        "]\n";
    char *shipped = temp_policy(shipped_text);
    char *user = temp_policy(user_text);
    AiPolicyBundle bundle;

    yew_test_capture_log();
    YEW_ASSERT(yew_ai_policy_load_paths(shipped, user, false, false,
                                        &bundle));
    YEW_ASSERT_EQ_U64(yew_ai_redact_policy_len(bundle.redact), 2U);
    YEW_ASSERT(policy_scan(bundle.redact, "SHIP12", "ship"));
    YEW_ASSERT(policy_scan(bundle.redact, "user34", "user"));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR, ":2: invalid AI deny row"));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR,
                                     ":3: invalid AI deny rule 'bad-regex'"));
    yew_ai_policy_bundle_drop(&bundle);
    YEW_ASSERT_EQ_I64(unlink(shipped), 0);
    YEW_ASSERT_EQ_I64(unlink(user), 0);
    free(shipped);
    free(user);
}

void test_ai_policy_replace_is_explicit_and_warns_about_dropped_defaults(void)
{
    const char *shipped_text =
        "[{ name: \"ship\", re: \"SHIP\", flags: \"\", note: \"shipped\" }]\n";
    const char *user_text =
        "[{ name: \"user\", re: \"USER\", flags: \"\", note: \"user\" }]\n";
    char *shipped = temp_policy(shipped_text);
    char *user = temp_policy(user_text);
    AiPolicyBundle bundle;

    yew_test_capture_log();
    YEW_ASSERT(yew_ai_policy_load_paths(shipped, user, true, true, &bundle));
    YEW_ASSERT_EQ_U64(yew_ai_redact_policy_len(bundle.redact), 1U);
    YEW_ASSERT(!policy_scan(bundle.redact, "SHIP", "ship"));
    YEW_ASSERT(policy_scan(bundle.redact, "USER", "user"));
    YEW_ASSERT_EQ_U64(yew_ai_path_policy_len(bundle.paths), 0U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "deny_replace dropped shipped"));
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "exclude_replace dropped shipped"));
    yew_ai_policy_bundle_drop(&bundle);
    YEW_ASSERT_EQ_I64(unlink(shipped), 0);
    YEW_ASSERT_EQ_I64(unlink(user), 0);
    free(shipped);
    free(user);
}
