#include "mod/ai/redact.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "search/regex.h"
#include "util/arena.h"

typedef struct AiCompiledRule {
    const char *name;
    YewRe *re;
    const char *required_literal;
    size_t required_literal_len;
    bool required_literal_fold;
} AiCompiledRule;

typedef struct AiRulePrefilter {
    const char *literal;
    bool fold;
} AiRulePrefilter;

struct AiRedactPolicy {
    Arena arena;
    AiCompiledRule *rules;
    size_t len;
};

struct AiPathPolicy {
    char **globs;
    size_t len;
};

static const AiRedactSpec shipped_redact[] = {
    {"aws-access-key", "\\b(A3T[A-Z0-9]|AKIA|ASIA|ABIA|ACCA|AGPA|AIDA|AIPA|ANPA|ANVA|AROA)[A-Z0-9]{16}\\b", 0U, "AWS access key id", "runtime/ai-deny.fl", 4U},
    {"aws-secret", "(?i:aws_?secret_?access_?key)[^\\n]{0,20}[:=][^\\n]{0,4}[A-Za-z0-9/+=]{40}", 0U, "labelled AWS secret", "runtime/ai-deny.fl", 5U},
    {"pem-private-key", "-----BEGIN [A-Z0-9 ]{0,32}PRIVATE KEY-----", 0U, "PEM private-key header", "runtime/ai-deny.fl", 6U},
    {"ssh-private-key", "\\bssh-rsa AAAA[A-Za-z0-9+/]{100,}", 0U, "SSH private-key blob", "runtime/ai-deny.fl", 7U},
    {"bearer-token", "(?i:bearer)[ \\t]+[A-Za-z0-9._~+/-]{16,}={0,2}", 0U, "bearer token", "runtime/ai-deny.fl", 8U},
    {"authorization-header", "(?i:authorization)[ \\t]*[:=][ \\t]*[\"']?[A-Za-z0-9._~+/-]{16,}", 0U, "authorization header", "runtime/ai-deny.fl", 9U},
    {"jwt", "\\beyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\b", 0U, "JSON web token", "runtime/ai-deny.fl", 10U},
    {"env-assignment", "(?i:^[ \\t]*(export[ \\t]+)?[A-Za-z_][A-Za-z0-9_]*(SECRET|TOKEN|PASSWORD|PASSWD|PRIVATE_KEY|API_?KEY|CREDENTIAL)[A-Za-z0-9_]*[ \\t]*=[ \\t]*[\"']?[^\\n\"' \\t]{8,})", 0U, "secret-named environment assignment", "runtime/ai-deny.fl", 11U},
    {"conn-string-creds", "\\b[a-z][a-z0-9+.-]{1,31}://[^\\n/:@ ]{1,64}:[^\\n/@ ]{1,128}@", 0U, "credentialed connection string", "runtime/ai-deny.fl", 12U},
    {"github-token", "\\bgh[pousr]_[A-Za-z0-9]{36,255}\\b", 0U, "GitHub token", "runtime/ai-deny.fl", 13U},
    {"slack-token", "\\bxox[baprs]-[A-Za-z0-9-]{10,}\\b", 0U, "Slack token", "runtime/ai-deny.fl", 14U},
    {"google-api-key", "\\bAIza[A-Za-z0-9_-]{35}\\b", 0U, "Google API key", "runtime/ai-deny.fl", 15U},
    {"openai-key", "\\bsk-[A-Za-z0-9_-]{20,}\\b", 0U, "OpenAI-compatible key", "runtime/ai-deny.fl", 16U},
    {"anthropic-key", "\\bsk-ant-[A-Za-z0-9_-]{20,}\\b", 0U, "Anthropic key", "runtime/ai-deny.fl", 17U},
    {"private-key-var", "(?i:private_?key)[ \\t]*[:=][ \\t]*[\"'][^\\n\"']{32,}", 0U, "private key literal", "runtime/ai-deny.fl", 18U},
    {"htpasswd-bcrypt", "\\$2[aby]?\\$[0-9]{2}\\$[A-Za-z0-9./]{53}", 0U, "bcrypt credential hash", "runtime/ai-deny.fl", 19U}
};

/* Each literal is present in every match of the corresponding shipped
 * expression.  Checking it first avoids running sixteen regex VMs over a
 * normal 4 KiB context while retaining the regex as the source of truth.
 * Exact expression matching below also gives runtime-loaded copies of the
 * shipped policy the same fast path without applying assumptions to user
 * expressions. */
static const AiRulePrefilter shipped_prefilters[] = {
    {"A", false},
    {"aws", true},
    {"PRIVATE KEY", false},
    {"ssh-rsa AAAA", false},
    {"bearer", true},
    {"authorization", true},
    {"eyJ", false},
    {"=", false},
    {"://", false},
    {"gh", false},
    {"xox", false},
    {"AIza", false},
    {"sk-", false},
    {"sk-ant-", false},
    {"private", true},
    {"$2", false}
};

_Static_assert(YEW_ARRAY_LEN(shipped_redact) ==
               YEW_ARRAY_LEN(shipped_prefilters),
               "shipped AI redaction prefilters must stay aligned");

static const char *const shipped_paths[] = {
    ".env*", "*secret*", "*credential*", ".ssh/", "*.pem", "*.key",
    "*.p12", "*.pfx", "*.jks", "*.keystore", "id_rsa*", "id_ecdsa*",
    "id_ed25519*", ".netrc", "_netrc", ".npmrc", ".pypirc", ".aws/",
    ".gnupg/", ".docker/config.json", "*.kdbx", "*.gpg", "*.asc"
};

static void clear_redact_error(AiRedactError *err)
{
    if (err != NULL)
        (void)memset(err, 0, sizeof(*err));
}

static void remember_redact_error(AiRedactError *out,
                                  const AiRedactSpec *spec,
                                  const YewReErr *re_err)
{
    if (out == NULL || out->message != NULL)
        return;
    out->source = spec->source;
    out->rule = spec->name;
    out->message = re_err->msg;
    out->line_1based = spec->line_1based;
    out->pattern_off = re_err->off;
}

static const AiRulePrefilter *prefilter_for(const AiRedactSpec *spec)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(shipped_redact); i++) {
        if (spec->flags == shipped_redact[i].flags &&
            strcmp(spec->re, shipped_redact[i].re) == 0)
            return &shipped_prefilters[i];
    }
    return NULL;
}

static bool add_redact_rule(AiRedactPolicy *policy, const AiRedactSpec *spec,
                            AiRedactError *err, bool required)
{
    const AiRulePrefilter *prefilter;
    YewReErr re_err = {0U, NULL};
    YewRe *re;

    if (spec->name == NULL || spec->re == NULL) {
        re_err.msg = "redaction row requires name and re";
        remember_redact_error(err, spec, &re_err);
        return !required;
    }
    re = yew_re_compile(&policy->arena, spec->re, strlen(spec->re),
                        spec->flags, &re_err);
    if (re == NULL) {
        remember_redact_error(err, spec, &re_err);
        return !required;
    }
    prefilter = prefilter_for(spec);
    policy->rules[policy->len].name = arena_strdup(&policy->arena, spec->name);
    policy->rules[policy->len].re = re;
    if (prefilter != NULL) {
        policy->rules[policy->len].required_literal = prefilter->literal;
        policy->rules[policy->len].required_literal_len =
            strlen(prefilter->literal);
        policy->rules[policy->len].required_literal_fold = prefilter->fold;
    }
    policy->len++;
    return true;
}

AiRedactPolicy *yew_ai_redact_policy_new(const AiRedactSpec *user,
                                         size_t user_len, bool replace,
                                         AiRedactError *err)
{
    AiRedactPolicy *policy;
    size_t cap = (replace ? 0U : YEW_ARRAY_LEN(shipped_redact)) + user_len;
    size_t i;

    clear_redact_error(err);
    if (user == NULL && user_len != 0U)
        return NULL;
    policy = yew_xcalloc(1U, sizeof(*policy));
    arena_init(&policy->arena);
    policy->rules = yew_xcalloc(cap == 0U ? 1U : cap, sizeof(*policy->rules));
    if (!replace) {
        for (i = 0U; i < YEW_ARRAY_LEN(shipped_redact); i++) {
            if (!add_redact_rule(policy, &shipped_redact[i], err, true)) {
                yew_ai_redact_policy_free(policy);
                return NULL;
            }
        }
    }
    for (i = 0U; i < user_len; i++)
        (void)add_redact_rule(policy, &user[i], err, false);
    return policy;
}

void yew_ai_redact_policy_free(AiRedactPolicy *policy)
{
    if (policy == NULL)
        return;
    arena_free_all(&policy->arena);
    yew_xfree(policy->rules);
    yew_xfree(policy);
}

size_t yew_ai_redact_policy_len(const AiRedactPolicy *policy)
{
    return policy == NULL ? 0U : policy->len;
}

const char *yew_ai_redact_policy_rule_name(const AiRedactPolicy *policy,
                                           size_t index)
{
    return policy == NULL || index >= policy->len ? NULL :
           policy->rules[index].name;
}

static u32 line_for_match(const AiCtx *ctx, bool prefix, u64 off)
{
    u64 lines = ctx->line_1based;
    u64 i;

    if (prefix) {
        for (i = off; i < ctx->plen; i++) {
            if (ctx->prefix[i] == (u8)'\n' && lines > 1U)
                lines--;
        }
    } else {
        for (i = 0U; i < off; i++) {
            if (ctx->suffix[i] == (u8)'\n' && lines < UINT32_MAX)
                lines++;
        }
    }
    return (u32)lines;
}

static u8 redact_ascii_fold(u8 c)
{
    return c >= (u8)'A' && c <= (u8)'Z' ? (u8)(c + ('a' - 'A')) : c;
}

static bool contains_literal(const u8 *bytes, size_t len,
                             const char *literal, size_t literal_len,
                             bool fold)
{
    size_t i;

    if (literal_len == 0U)
        return true;
    if (bytes == NULL || literal_len > len)
        return false;
    for (i = 0U; i <= len - literal_len; i++) {
        size_t j;

        for (j = 0U; j < literal_len; j++) {
            u8 have = bytes[i + j];
            u8 want = (u8)literal[j];

            if (fold) {
                have = redact_ascii_fold(have);
                want = redact_ascii_fold(want);
            }
            if (have != want)
                break;
        }
        if (j == literal_len)
            return true;
    }
    return false;
}

static bool scan_half(const AiCompiledRule *rule, const AiCtx *ctx,
                      bool prefix, RedactHit *hit)
{
    const u8 *bytes = prefix ? ctx->prefix : ctx->suffix;
    u32 len = prefix ? ctx->plen : ctx->slen;
    YewReInput input;
    YewReMatch match;
    u64 match_len;

    if (bytes == NULL || len == 0U)
        return false;
    if (!contains_literal(bytes, len, rule->required_literal,
                          rule->required_literal_len,
                          rule->required_literal_fold))
        return false;
    input = yew_re_input_bytes(bytes, len);
    if (!yew_re_search(rule->re, &input, BYTEOFF(0U), &match))
        return false;
    match_len = match.g[0].hi - match.g[0].lo;
    if (match_len > UINT32_MAX)
        return false;
    if (hit != NULL) {
        hit->rule = rule->name;
        hit->line_1based = line_for_match(ctx, prefix, match.g[0].lo);
        hit->off = BYTEOFF(match.g[0].lo);
        hit->len = (u32)match_len;
        hit->in_prefix = prefix;
    }
    return true;
}

bool yew_ai_redact_scan(const AiRedactPolicy *policy, const AiCtx *ctx,
                        RedactHit *hit)
{
    size_t i;

    if (policy == NULL || ctx == NULL)
        return false;
    for (i = 0U; i < policy->len; i++) {
        if (scan_half(&policy->rules[i], ctx, true, hit) ||
            scan_half(&policy->rules[i], ctx, false, hit))
            return true;
    }
    return false;
}

static u8 ascii_fold(u8 c)
{
    return c >= (u8)'A' && c <= (u8)'Z' ? (u8)(c + ('a' - 'A')) : c;
}

bool yew_ai_path_glob_valid(const char *glob)
{
    return glob != NULL && glob[0] != '\0' && strstr(glob, "**") == NULL;
}

static bool glob_match_n(const char *glob, size_t glob_len,
                         const char *text, size_t text_len)
{
    const u8 *p = (const u8 *)glob;
    const u8 *pend = p + glob_len;
    const u8 *s = (const u8 *)text;
    const u8 *send = s + text_len;
    const u8 *star = NULL;
    const u8 *retry = NULL;

    while (s < send) {
        if (p < pend && *p == (u8)'*') {
            star = ++p;
            retry = s;
        } else if (p < pend && *p == (u8)'?' && *s != (u8)'/') {
            p++;
            s++;
        } else if (p < pend && ascii_fold(*p) == ascii_fold(*s)) {
            p++;
            s++;
        } else if (star != NULL && retry < send && *retry != (u8)'/') {
            p = star;
            s = ++retry;
        } else {
            return false;
        }
    }
    while (p < pend && *p == (u8)'*')
        p++;
    return p == pend;
}

static bool glob_match(const char *glob, const char *text)
{
    return glob_match_n(glob, strlen(glob), text, strlen(text));
}

static bool component_match(const char *path, const char *glob,
                            size_t glob_len)
{
    const char *at = path;

    for (;;) {
        const char *slash = strchr(at, '/');
        size_t len = slash == NULL ? strlen(at) : (size_t)(slash - at);

        if (glob_match_n(glob, glob_len, at, len))
            return true;
        if (slash == NULL)
            return false;
        at = slash + 1;
    }
}

static bool path_matches(const char *path, const char *glob)
{
    size_t glen = strlen(glob);
    const char *base = strrchr(path, '/');

    base = base == NULL ? path : base + 1;
    if (glen > 0U && glob[glen - 1U] == '/')
        return component_match(path, glob, glen - 1U);
    return glob_match(glob, path) || glob_match(glob, base) ||
           component_match(path, glob, glen);
}

static char *string_copy(const char *s)
{
    size_t len = strlen(s);
    char *copy = yew_xmalloc(len + 1U);

    (void)memcpy(copy, s, len + 1U);
    return copy;
}

AiPathPolicy *yew_ai_path_policy_new(const char *const *user,
                                     size_t user_len, bool replace,
                                     AiPathError *err)
{
    AiPathPolicy *policy;
    size_t cap = (replace ? 0U : YEW_ARRAY_LEN(shipped_paths)) + user_len;
    size_t i;

    if (err != NULL)
        (void)memset(err, 0, sizeof(*err));
    if (user == NULL && user_len != 0U)
        return NULL;
    policy = yew_xcalloc(1U, sizeof(*policy));
    policy->globs = yew_xcalloc(cap == 0U ? 1U : cap, sizeof(*policy->globs));
    if (!replace) {
        for (i = 0U; i < YEW_ARRAY_LEN(shipped_paths); i++)
            policy->globs[policy->len++] = string_copy(shipped_paths[i]);
    }
    for (i = 0U; i < user_len; i++) {
        if (!yew_ai_path_glob_valid(user[i])) {
            if (err != NULL && err->message == NULL) {
                err->pattern = user[i];
                err->message = "recursive '**' globs are not supported";
                err->index = i;
            }
            continue;
        }
        policy->globs[policy->len++] = string_copy(user[i]);
    }
    return policy;
}

void yew_ai_path_policy_free(AiPathPolicy *policy)
{
    size_t i;

    if (policy == NULL)
        return;
    for (i = 0U; i < policy->len; i++)
        yew_xfree(policy->globs[i]);
    yew_xfree(policy->globs);
    yew_xfree(policy);
}

size_t yew_ai_path_policy_len(const AiPathPolicy *policy)
{
    return policy == NULL ? 0U : policy->len;
}

bool yew_ai_path_excluded(const AiPathPolicy *policy, const char *path,
                          AiPathHit *hit)
{
    size_t i;

    if (policy == NULL || path == NULL)
        return false;
    for (i = 0U; i < policy->len; i++) {
        if (path_matches(path, policy->globs[i])) {
            if (hit != NULL)
                hit->pattern = policy->globs[i];
            return true;
        }
    }
    return false;
}
