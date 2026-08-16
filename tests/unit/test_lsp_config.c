#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/flconf.h"
#include "mod/lsp/client.h"
#include "mod/lsp/lsp.h"

typedef struct LspConfigFix {
    Ed ed;
    char path[96];
} LspConfigFix;

static void write_config(const char *path, const char *source)
{
    FILE *fp = fopen(path, "wb");
    size_t len = strlen(source);

    if (fp == NULL || fwrite(source, 1U, len, fp) != len || fclose(fp) != 0)
        YEW_BUG("LSP config fixture write failed");
}

static void fix_init(LspConfigFix *f, const char *source)
{
    YewEdStartup startup = {0};
    int fd;

    (void)memset(f, 0, sizeof(*f));
    (void)snprintf(f->path, sizeof(f->path), "/tmp/yew-lsp-config-XXXXXX");
    fd = mkstemp(f->path);
    if (fd < 0 || close(fd) != 0)
        YEW_BUG("LSP config fixture mkstemp failed");
    write_config(f->path, source);
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    startup.config_path = f->path;
    startup.no_workspace_config = true;
    yew_config_init(&f->ed, &startup);
    YEW_ASSERT_EQ_I64(yew_config_load_all(&f->ed, NULL), YEW_CFG_OK);
}

static void fix_free(LspConfigFix *f)
{
    yew_ed_free(&f->ed);
    (void)unlink(f->path);
}

void test_lsp_config_defaults_and_replacement_semantics(void)
{
    LspConfigFix f;
    const LspServerCfg *cfg;

    fix_init(&f, "\n");
    cfg = yew_lsp_client_cfg(&f.ed, "c");
    YEW_ASSERT_NOT_NULL(cfg);
    YEW_ASSERT_EQ_STR(cfg->id, "clangd");
    YEW_ASSERT_NULL(yew_lsp_client_cfg(&f.ed, "wolf"));
    fix_free(&f);

    fix_init(&f,
        "let lsp = {servers: {wolf: {id: \"wolfd\", cmd: \"wolf-lsp\", "
        "args: [\"serve\", \"--stdio\"], roots: [\"wolf.toml\", \".git\"], "
        "init_options: \"{\\\"dialect\\\":\\\"flow\\\"}\", "
        "init_timeout_ms: 4321}}}\n");
    cfg = yew_lsp_client_cfg(&f.ed, "wolf");
    YEW_ASSERT_NOT_NULL(cfg);
    YEW_ASSERT_EQ_STR(cfg->lang, "wolf");
    YEW_ASSERT_EQ_STR(cfg->id, "wolfd");
    YEW_ASSERT_EQ_STR(cfg->cmd, "wolf-lsp");
    YEW_ASSERT_EQ_STR(cfg->args[0], "serve");
    YEW_ASSERT_EQ_STR(cfg->args[1], "--stdio");
    YEW_ASSERT_NULL(cfg->args[2]);
    YEW_ASSERT_EQ_STR(cfg->roots[0], "wolf.toml");
    YEW_ASSERT_EQ_STR(cfg->roots[1], ".git");
    YEW_ASSERT_NULL(cfg->roots[2]);
    YEW_ASSERT_EQ_STR(cfg->init_options, "{\"dialect\":\"flow\"}");
    YEW_ASSERT_EQ_I64(cfg->init_timeout_ms, 4321);
    YEW_ASSERT_NULL(yew_lsp_client_cfg(&f.ed, "c"));
    fix_free(&f);
}

void test_lsp_config_is_owned_and_refreshes_after_reload(void)
{
    LspConfigFix f;
    const LspServerCfg *cfg;

    fix_init(&f,
        "let lsp = {servers: {wolf: {id: \"first\", cmd: \"wolf-one\", "
        "args: [\"old\"], roots: [\"wolf.toml\"]}}}\n");
    cfg = yew_lsp_client_cfg(&f.ed, "wolf");
    YEW_ASSERT_NOT_NULL(cfg);
    YEW_ASSERT_EQ_STR(cfg->cmd, "wolf-one");

    write_config(f.path,
        "let lsp = {servers: {wolf: {id: \"second\", cmd: \"wolf-two\", "
        "args: [\"new\"], roots: [\".git\"]}}}\n");
    YEW_ASSERT_EQ_I64(yew_config_reload(&f.ed, NULL), YEW_CFG_OK);
    /* The old Fletch closure has been unrooted and collected here.  The
     * resolver detects the config generation change before dereferencing
     * any of its old host-owned rows. */
    cfg = yew_lsp_client_cfg(&f.ed, "wolf");
    YEW_ASSERT_NOT_NULL(cfg);
    YEW_ASSERT_EQ_STR(cfg->id, "second");
    YEW_ASSERT_EQ_STR(cfg->cmd, "wolf-two");
    YEW_ASSERT_EQ_STR(cfg->args[0], "new");
    YEW_ASSERT_EQ_STR(cfg->roots[0], ".git");
    fix_free(&f);
}

void test_lsp_config_rejects_invalid_rows_without_losing_valid_rows(void)
{
    LspConfigFix f;
    const LspServerCfg *cfg;

    yew_test_capture_log();
    fix_init(&f,
        "let lsp = {servers: {"
        "nul: {id: \"bad\\0id\", cmd: \"x\"}, "
        "timeout: {id: \"bad-time\", cmd: \"x\", init_timeout_ms: 0}, "
        "json: {id: \"bad-json\", cmd: \"x\", init_options: \"[]\"}, "
        "roots: {id: \"bad-roots\", cmd: \"x\", roots: \".git\"}, "
        "good: {id: \"goodd\", cmd: \"good-lsp\", init_options: nil, "
        "init_timeout_ms: 600000}}}\n");
    YEW_ASSERT_NULL(yew_lsp_client_cfg(&f.ed, "nul"));
    YEW_ASSERT_NULL(yew_lsp_client_cfg(&f.ed, "timeout"));
    YEW_ASSERT_NULL(yew_lsp_client_cfg(&f.ed, "json"));
    YEW_ASSERT_NULL(yew_lsp_client_cfg(&f.ed, "roots"));
    cfg = yew_lsp_client_cfg(&f.ed, "good");
    YEW_ASSERT_NOT_NULL(cfg);
    YEW_ASSERT_EQ_STR(cfg->cmd, "good-lsp");
    YEW_ASSERT_EQ_I64(cfg->init_timeout_ms, 600000);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                    "LSP config: ignoring server nul"));
    YEW_ASSERT(f.ed.msg.active);
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_WARN);
    fix_free(&f);
}

void test_lsp_config_defers_startup_and_does_not_retry_disabled_language(void)
{
    YewEdStartup startup = {0};
    Ed ed;
    char config[] = "/tmp/yew-lsp-disabled-config-XXXXXX";
    char document[] = "/tmp/yew-lsp-disabled-doc-XXXXXX";
    int config_fd = mkstemp(config);
    int doc_fd = mkstemp(document);
    u32 i;

    if (config_fd < 0 || doc_fd < 0 || close(config_fd) != 0 ||
        close(doc_fd) != 0)
        YEW_BUG("LSP disabled fixture creation failed");
    write_config(config, "let lsp = {servers: {}}\n");
    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(yew_ed_open(&ed, document), YEW_LOAD_OK);
    ed.buffer.lang = "c";
    YEW_ASSERT_NULL(ed.lsp); /* hydrate/open precedes init.fl */

    startup.config_path = config;
    startup.no_workspace_config = true;
    yew_config_init(&ed, &startup);
    YEW_ASSERT_EQ_I64(yew_config_load_all(&ed, NULL), YEW_CFG_OK);
    for (i = 0U; i < 20U; i++)
        yew_lsp_pump(&ed);
    YEW_ASSERT_NULL(yew_lsp_doc_for_buffer(&ed, &ed.buffer));
    YEW_ASSERT(!ed.msg.active);
    yew_ed_free(&ed);
    (void)unlink(document);
    (void)unlink(config);
}
