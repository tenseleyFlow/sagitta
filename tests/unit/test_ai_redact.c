#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/ai/redact.h"
#include "util/base.h"

typedef struct RedactCase {
    const char *rule;
    const char *positive[3];
    const char *negative[3];
} RedactCase;

static const RedactCase cases[] = {
    {"aws-access-key",
     {"AKIA" "ABCDEFGHIJKLMNOP", "x ASIA" "0123456789ABCDEF y",
      "AROA" "9999999999999999"},
     {"AKIA" "ABCDEFGHIJKLMNO", "AKIA" "abcdefghijklmnop",
      "XAKIA" "ABCDEFGHIJKLMNOPX"}},
    {"aws-secret",
     {"AWS_SECRET_" "ACCESS_KEY=0123456789012345678901234567890123456789",
      "awsSecret" "AccessKey : ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/==",
      "aws_secret_" "access_key----:AAAA1111BBBB2222CCCC3333DDDD4444EEEE5555"},
     {"0123456789012345678901234567890123456789",
      "prefix aws_secret_" "access_key=012345678901234567890123456789012345678",
      "prefix aws_secret_" "access_key................................=0123456789012345678901234567890123456789"}},
    {"pem-private-key",
     {"-----BEGIN PRIVATE KEY-----", "-----BEGIN RSA PRIVATE KEY-----",
      "-----BEGIN OPENSSH PRIVATE KEY-----"},
     {"-----BEGIN PUBLIC KEY-----", "-----END PRIVATE KEY-----",
      "-----BEGIN lowercase PRIVATE KEY-----"}},
    {"ssh-private-key",
     {"ssh-rsa AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      "x ssh-rsa AAAA1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111",
      "ssh-rsa AAAA++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"},
     {"ssh-ed25519 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      "ssh-rsa AAAAshort", "xssh-rsa AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}},
    {"bearer-token",
     {"Bearer abcdefghijklmnop", "bearer\tabcdEFGH.0123_~+-", "BEARER token-token-token=="},
     {"Bearer short", "bearerabcdefghijklmnop", "beaker abcdefghijklmnop"}},
    {"authorization-header",
     {"Authorization: abcdefghijklmnop", "authorization=\"abcdEFGH0123_~+-\"",
      "AUTHORIZATION : token-token-token"},
     {"Authorization: short", "Authorization abcdefghijklmnop", "Authentication: abcdefghijklmnop"}},
    {"jwt",
     {"eyJabcdefgh.ijklmnop.qrstuvwx", "x eyJ0123456789.ABCDEFGHIJ.________ y",
      "eyJheader_1.payload_2.signature3"},
     {"eyJshort.ijklmnop.qrstuvwx", "eyJabcdefgh.ijklmnop", "xeyJabcdefgh.ijklmnop.qrstuvwxz"}},
    {"env-assignment",
     {"MY_SECRET=abcdefgh", " export SERVICE_API_KEY = '0123456789'", "MY_PASSWORD_VALUE=long-enough"},
     {"MY_SECRET=short", "NOT_PRIVATE=abcdefgh", "prefix MY_TOKEN=abcdefgh"}},
    {"conn-string-creds",
     {"postgres://user:password@host/db", "amqp+ssl://guest:hunter2@example", "x redis://u:p@localhost y"},
     {"postgres://host/db", "postgres://user@host/db", "postgres://:password@host"}},
    {"github-token",
     {"ghp_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", "gho_abcdefghijklmnopqrstuvwxyz0123456789",
      "x ghr_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA y"},
     {"ghp_0123456789ABCDEFGHIJKLMNOPQRSTUVWXY", "ghx_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
      "xghp_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZy"}},
    {"slack-token",
     {"xoxb-1234567890", "xoxa-abc-def-123", "x xoxr-ABCDEFGHIJK y"},
     {"xoxz-1234567890", "xoxb-123456789", "xxoxb-1234567890x"}},
    {"google-api-key",
     {"AIza0123456789ABCDEFGHIJKLMNOPQRSTUVWXY", "x AIzaabcdefghijklmnopqrstuvwxyz123456789 y",
      "AIza___________________________________"},
     {"AIza0123456789ABCDEFGHIJKLMNOPQRSTUVWX", "AIza0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
      "xAIza0123456789ABCDEFGHIJKLMNOPQRSTUVWXYz"}},
    {"openai-key",
     {"sk-0123456789ABCDEFGHIJ", "x sk-abcdefghijklmnopqrstuvwxyz y", "sk-____________________"},
     {"sk-0123456789ABCDEFGHI", "SK-0123456789ABCDEFGHIJ", "xsk-0123456789ABCDEFGHIJz"}},
    {"anthropic-key",
     {"sk-ant-0123456789ABCDEFGHIJ", "x sk-ant-abcdefghijklmnopqrstuvwxyz y", "sk-ant-____________________"},
     {"ant-0123456789ABCDEFGHIJ", "SK-ant-0123456789ABCDEFGHIJ", "xsk-ant-0123456789ABCDEFGHIJz"}},
    {"private-key-var",
     {"private_key='01234567890123456789012345678901'", "PRIVATEKEY: \"abcdefghijklmnopqrstuvwxyzABCDEF\"",
      "private_key = 'a deliberately long private key value'"},
     {"public_key='01234567890123456789012345678901'", "private_key=unquoted01234567890123456789012345678901",
      "private_key='0123456789012345678901234567890'"}},
    {"htpasswd-bcrypt",
     {"$2b$12$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq",
      "$2a$04$ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopq",
      "$2y$31$....................................................."},
     {"$2z$12$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmno",
      "$2b$1$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmno",
      "$2b$12$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn"}}
};

static bool scan_text(const AiRedactPolicy *policy, const char *text,
                      AiRedactHit *hit)
{
    AiCtx ctx;

    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.prefix = (const u8 *)text;
    ctx.plen = (u32)strlen(text);
    ctx.line_1based = 1U;
    return yew_ai_redact_scan(policy, &ctx, hit);
}

void test_ai_redact_shipped_fixture_matrix(void)
{
    AiRedactPolicy *policy = yew_ai_redact_policy_new(NULL, 0U, false, NULL);
    size_t i;
    size_t j;

    YEW_ASSERT_NOT_NULL(policy);
    YEW_ASSERT_EQ_U64(yew_ai_redact_policy_len(policy), 16U);
    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        for (j = 0U; j < 3U; j++) {
            AiRedactHit hit;

            if (!scan_text(policy, cases[i].positive[j], &hit))
                (void)fprintf(stderr, "redaction fixture missed %s positive %lu\n",
                              cases[i].rule, (unsigned long)j);
            YEW_ASSERT(scan_text(policy, cases[i].positive[j], &hit));
            if (strcmp(cases[i].rule, "anthropic-key") == 0)
                YEW_ASSERT_EQ_STR(hit.rule, "openai-key");
            else
                YEW_ASSERT_EQ_STR(hit.rule, cases[i].rule);
            YEW_ASSERT(hit.len > 0U);
        }
        for (j = 0U; j < 3U; j++) {
            if (scan_text(policy, cases[i].negative[j], NULL))
                (void)fprintf(stderr, "redaction fixture hit %s negative %lu\n",
                              cases[i].rule, (unsigned long)j);
            YEW_ASSERT(!scan_text(policy, cases[i].negative[j], NULL));
        }
    }
    yew_ai_redact_policy_free(policy);
}

void test_ai_redact_false_positive_boundaries(void)
{
    static const char cjk[] =
        "秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密秘密";
    AiRedactPolicy *policy = yew_ai_redact_policy_new(NULL, 0U, false, NULL);

    YEW_ASSERT_NOT_NULL(policy);
    YEW_ASSERT(!scan_text(policy, cjk, NULL));
    YEW_ASSERT(!scan_text(policy, "0123456789abcdef0123456789abcdef01234567", NULL));
    YEW_ASSERT(!scan_text(policy, "AKIA" "123456789012345", NULL));
    yew_ai_redact_policy_free(policy);
}

void test_ai_redact_prefix_suffix_line_and_first_rule(void)
{
    AiRedactPolicy *policy = yew_ai_redact_policy_new(NULL, 0U, false, NULL);
    const char prefix[] = "one\ntwo\nAKIA" "ABCDEFGHIJKLMNOP";
    const char suffix[] = "tail\nBearer abcdefghijklmnop";
    AiCtx ctx;
    AiRedactHit hit;

    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.prefix = (const u8 *)prefix;
    ctx.plen = (u32)strlen(prefix);
    ctx.suffix = (const u8 *)suffix;
    ctx.slen = (u32)strlen(suffix);
    ctx.line_1based = 3U;
    YEW_ASSERT(yew_ai_redact_scan(policy, &ctx, &hit));
    YEW_ASSERT_EQ_STR(hit.rule, "aws-access-key");
    YEW_ASSERT(hit.in_prefix);
    YEW_ASSERT_EQ_U64(hit.line_1based, 3U);
    YEW_ASSERT_EQ_U64(hit.off.v, 8U);

    ctx.prefix = NULL;
    ctx.plen = 0U;
    YEW_ASSERT(yew_ai_redact_scan(policy, &ctx, &hit));
    YEW_ASSERT_EQ_STR(hit.rule, "bearer-token");
    YEW_ASSERT(!hit.in_prefix);
    YEW_ASSERT_EQ_U64(hit.line_1based, 4U);
    YEW_ASSERT_EQ_U64(hit.off.v, 5U);
    yew_ai_redact_policy_free(policy);
}

void test_ai_redact_user_merge_replace_and_compile_error(void)
{
    const AiRedactSpec user[] = {
        {"custom", "WOLF_[A-Z]{8}", 0U, "custom language secret", "user.fl", 7U},
        {"broken", "(", 0U, "invalid", "user.fl", 8U}
    };
    AiRedactError err;
    AiRedactPolicy *merged = yew_ai_redact_policy_new(user, 2U, false, &err);
    AiRedactPolicy *replaced = yew_ai_redact_policy_new(user, 1U, true, NULL);
    AiRedactHit hit;

    YEW_ASSERT_NOT_NULL(merged);
    YEW_ASSERT_EQ_U64(yew_ai_redact_policy_len(merged), 17U);
    YEW_ASSERT_EQ_STR(err.source, "user.fl");
    YEW_ASSERT_EQ_U64(err.line_1based, 8U);
    YEW_ASSERT_NOT_NULL(err.message);
    YEW_ASSERT(scan_text(merged, "AKIA" "ABCDEFGHIJKLMNOP", &hit));
    YEW_ASSERT_EQ_STR(hit.rule, "aws-access-key");
    YEW_ASSERT(scan_text(merged, "WOLF_ABCDEFGH", &hit));
    YEW_ASSERT_EQ_STR(hit.rule, "custom");

    YEW_ASSERT_NOT_NULL(replaced);
    YEW_ASSERT_EQ_U64(yew_ai_redact_policy_len(replaced), 1U);
    YEW_ASSERT(!scan_text(replaced, "AKIA" "ABCDEFGHIJKLMNOP", NULL));
    YEW_ASSERT(scan_text(replaced, "WOLF_ABCDEFGH", &hit));
    yew_ai_redact_policy_free(merged);
    yew_ai_redact_policy_free(replaced);
}
