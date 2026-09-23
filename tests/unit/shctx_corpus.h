#ifndef YEW_TEST_SHCTX_CORPUS_H
#define YEW_TEST_SHCTX_CORPUS_H

/*
 * Sprint 57.23 Testing Strategy: the context-lexer corpus.
 *
 * One row per case.  `in` is a `:!` body with the caret written as `‸`
 * (U+2038, which no row otherwise contains).  `raw` is the text the
 * completion REPLACES -- line[replace.lo, caret) -- and `stem` is its
 * decoded form, so every row pins both halves of `replace`.
 *
 * `argv0` NULL means argc == 0 (command position, ASSIGN, NONE); "" means
 * the operands of no command (`for x in …`, `fi > out`).
 *
 * Shared with tests/fuzz/fuzz_shctx.c, whose seed corpus it is.
 */

#include "ui/shctx.h"
#include "util/base.h"

#define SC_CMD YEW_SH_POS_COMMAND
#define SC_ARG YEW_SH_POS_ARGUMENT
#define SC_RED YEW_SH_POS_REDIRECT
#define SC_ASG YEW_SH_POS_ASSIGN
#define SC_VAR YEW_SH_POS_VARIABLE
#define SC_NON YEW_SH_POS_NONE

#define SC_QN YEW_SH_Q_NONE
#define SC_QS YEW_SH_Q_SINGLE
#define SC_QD YEW_SH_Q_DOUBLE
#define SC_QL YEW_SH_Q_DOLLAR

enum {
    SC_DASHDASH = 1U << 0,
    SC_BRACE = 1U << 1,
    SC_TILDE = 1U << 2,
    SC_EXPANDS = 1U << 3
};

typedef struct ShCorpusRow {
    const char *in;
    u8 pos;
    u8 quote;
    const char *argv0;
    u32 arg_index;
    const char *raw;
    const char *stem;
    u8 flags;
    u32 depth;
} ShCorpusRow;

static const ShCorpusRow sh_corpus[] = {
    /* -- the contract's samples, in its order ------------------- */
    {"./scr‸", SC_CMD, SC_QN, NULL, 0, "./scr", "./scr", 0, 0},
    {"ls | gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0, 0},
    {"make && ./bu‸", SC_CMD, SC_QN, NULL, 0, "./bu", "./bu", 0, 0},
    {"FOO=1 BAR=2 cm‸", SC_CMD, SC_QN, NULL, 0, "cm", "cm", 0, 0},
    {"sudo -u root gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"echo $HO‸", SC_VAR, SC_QN, "echo", 1, "HO", "HO", 0, 0},
    {"echo \"${PA‸", SC_VAR, SC_QD, "echo", 1, "PA", "PA", SC_BRACE, 0},
    {"echo '$HO‸", SC_ARG, SC_QS, "echo", 1, "'$HO", "$HO", 0, 0},
    {"cat > ou‸", SC_RED, SC_QN, "cat", 1, "ou", "ou", 0, 0},
    {"cat 2>er‸", SC_RED, SC_QN, "cat", 1, "er", "er", 0, 0},
    {"grep x $(ls sr‸", SC_ARG, SC_QN, "ls", 1, "sr", "sr", 0, 1},
    {"echo $(wh‸", SC_CMD, SC_QN, NULL, 0, "wh", "wh", 0, 1},
    {"cd \"my di‸", SC_ARG, SC_QD, "cd", 1, "\"my di", "my di", 0, 0},
    {"git commit -m \"fix # not‸", SC_ARG, SC_QD, "git", 3,
     "\"fix # not", "fix # not", 0, 0},
    {"ls # comm‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"cat <<EO‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"rm -- -r‸", SC_ARG, SC_QN, "rm", 2, "-r", "-r", SC_DASHDASH, 0},
    {"PATH=/usr/b‸", SC_ASG, SC_QN, NULL, 0, "/usr/b", "/usr/b", 0, 0},

    /* -- path-shaped command words (row 4 of §3) ---------------- */
    {"../bu‸", SC_CMD, SC_QN, NULL, 0, "../bu", "../bu", 0, 0},
    {"~/bin/sc‸", SC_CMD, SC_QN, NULL, 0, "~/bin/sc", "~/bin/sc",
     SC_TILDE, 0},
    {"/usr/bin/gi‸", SC_CMD, SC_QN, NULL, 0, "/usr/bin/gi", "/usr/bin/gi",
     0, 0},
    {".‸", SC_CMD, SC_QN, NULL, 0, ".", ".", 0, 0},
    {"..‸", SC_CMD, SC_QN, NULL, 0, "..", "..", 0, 0},
    {"bin/‸", SC_CMD, SC_QN, NULL, 0, "bin/", "bin/", 0, 0},
    {"make && ../x/‸", SC_CMD, SC_QN, NULL, 0, "../x/", "../x/", 0, 0},
    {"./my\\ scr‸", SC_CMD, SC_QN, NULL, 0, "./my\\ scr", "./my scr", 0, 0},
    {"\"./my s‸", SC_CMD, SC_QD, NULL, 0, "\"./my s", "./my s", 0, 0},
    {"'~/x‸", SC_CMD, SC_QS, NULL, 0, "'~/x", "~/x", 0, 0},

    /* -- after control operators ------------------------------- */
    {"ls|gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0, 0},
    {"ls || gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0, 0},
    {"ls |& gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0, 0},
    {"a; b‸", SC_CMD, SC_QN, NULL, 0, "b", "b", 0, 0},
    {"a;b‸", SC_CMD, SC_QN, NULL, 0, "b", "b", 0, 0},
    {"sleep 1 & gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"a\ngi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"ls |‸", SC_CMD, SC_QN, NULL, 0, "", "", 0, 0},
    {"‸", SC_CMD, SC_QN, NULL, 0, "", "", 0, 0},
    {"  ‸", SC_CMD, SC_QN, NULL, 0, "", "", 0, 0},
    {"ls‸ | grep", SC_CMD, SC_QN, NULL, 0, "ls", "ls", 0, 0},
    {"ls |‸ grep", SC_CMD, SC_QN, NULL, 0, "", "", 0, 0},
    {"a &&‸", SC_CMD, SC_QN, NULL, 0, "", "", 0, 0},
    {"a ;; b‸", SC_CMD, SC_QN, NULL, 0, "b", "b", 0, 0},

    /* -- reserved words and groups ------------------------------ */
    {"(gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 1},
    {"( cd x; gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 1},
    {"{ gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 1},
    {"if gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"if true; then gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"while read l; do ec‸", SC_CMD, SC_QN, NULL, 0, "ec", "ec", 0, 0},
    {"! gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"time gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"until false; do gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"if a; then b; else c‸", SC_CMD, SC_QN, NULL, 0, "c", "c", 0, 0},
    {"if a; then b; elif c‸", SC_CMD, SC_QN, NULL, 0, "c", "c", 0, 0},
    {"echo if‸", SC_ARG, SC_QN, "echo", 1, "if", "if", 0, 0},
    {"{ ls; } > ou‸", SC_RED, SC_QN, "", 1, "ou", "ou", 0, 0},
    {"{ ls; }; gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"(ls) fo‸", SC_ARG, SC_QN, "", 1, "fo", "fo", 0, 0},

    /* -- assignment prefixes and values ------------------------ */
    {"FOO=‸", SC_ASG, SC_QN, NULL, 0, "", "", 0, 0},
    {"FOO=bar BAZ=qu‸", SC_ASG, SC_QN, NULL, 0, "qu", "qu", 0, 0},
    {"FOO=~/x‸", SC_ASG, SC_QN, NULL, 0, "~/x", "~/x", SC_TILDE, 0},
    {"FOO=\"a b‸", SC_ASG, SC_QD, NULL, 0, "\"a b", "a b", 0, 0},
    {"FOO=$HOME/x‸", SC_ASG, SC_QN, NULL, 0, "$HOME/x", "/x",
     SC_EXPANDS, 0},
    {"FOO=1 ls BAR=x‸", SC_ARG, SC_QN, "ls", 1, "BAR=x", "BAR=x", 0, 0},
    {"echo FOO=b‸", SC_ARG, SC_QN, "echo", 1, "FOO=b", "FOO=b", 0, 0},
    {"env FOO=ba‸", SC_ASG, SC_QN, NULL, 0, "ba", "ba", 0, 0},
    {"_X9=v‸", SC_ASG, SC_QN, NULL, 0, "v", "v", 0, 0},
    {"9X=v‸", SC_CMD, SC_QN, NULL, 0, "9X=v", "9X=v", 0, 0},
    {"'FOO'=v‸", SC_CMD, SC_QN, NULL, 0, "'FOO'=v", "FOO=v", 0, 0},
    {"A=1 B=2 C=3 ‸", SC_CMD, SC_QN, NULL, 0, "", "", 0, 0},
    {"A=1 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"A=1 B='x y' gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"A=$(x) gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"A=1\tgi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"A= gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"A=1 B=2 ./x‸", SC_CMD, SC_QN, NULL, 0, "./x", "./x", 0, 0},
    {"_=1 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"A=1 sudo B=2 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},

    /* -- precommand wrappers (§2) ------------------------------- */
    {"sudo gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"sudo -E gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"sudo -u ro‸", SC_ARG, SC_QN, "sudo", 2, "ro", "ro", 0, 0},
    {"sudo -‸", SC_ARG, SC_QN, "sudo", 1, "-", "-", 0, 0},
    {"sudo -- -x‸", SC_CMD, SC_QN, NULL, 0, "-x", "-x", 0, 0},
    {"doas -u root gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"env -i FOO=1 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"env -u HOME gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"nice -n 10 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"nice gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"timeout 5 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"timeout -s KILL 5 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"timeout 5‸", SC_ARG, SC_QN, "timeout", 1, "5", "5", 0, 0},
    {"timeout -k 1 3‸", SC_ARG, SC_QN, "timeout", 3, "3", "3", 0, 0},
    {"xargs -n 1 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"xargs -I {} gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"nohup gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"command -v gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"builtin ec‸", SC_CMD, SC_QN, NULL, 0, "ec", "ec", 0, 0},
    {"exec gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"caffeinate -i gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"unbuffer gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"sudo env FOO=1 nice -n 5 gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi",
     0, 0},
    {"sudo git st‸", SC_ARG, SC_QN, "git", 1, "st", "st", 0, 0},
    {"FOO=1 sudo gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"'sudo' gi‸", SC_ARG, SC_QN, "sudo", 1, "gi", "gi", 0, 0},
    {"sudo -u root git commit -‸", SC_ARG, SC_QN, "git", 2, "-", "-", 0, 0},
    {"ls | sudo gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},

    /* -- variables ----------------------------------------------- */
    {"echo $‸", SC_VAR, SC_QN, "echo", 1, "", "", 0, 0},
    {"echo ${‸", SC_VAR, SC_QN, "echo", 1, "", "", SC_BRACE, 0},
    {"$HO‸", SC_VAR, SC_QN, NULL, 0, "HO", "HO", 0, 0},
    {"echo \"$HO‸", SC_VAR, SC_QD, "echo", 1, "HO", "HO", 0, 0},
    {"echo a$HO‸", SC_VAR, SC_QN, "echo", 1, "HO", "HO", 0, 0},
    {"echo $HOME/fo‸", SC_ARG, SC_QN, "echo", 1, "$HOME/fo", "/fo",
     SC_EXPANDS, 0},
    {"echo ${HOME}/fo‸", SC_ARG, SC_QN, "echo", 1, "${HOME}/fo", "/fo",
     SC_EXPANDS, 0},
    {"echo $1‸", SC_ARG, SC_QN, "echo", 1, "$1", "", SC_EXPANDS, 0},
    {"cat > $OU‸", SC_VAR, SC_QN, "cat", 1, "OU", "OU", 0, 0},
    {"echo ${#HO‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"echo ${HOME:-x‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"FOO=$BA‸", SC_VAR, SC_QN, NULL, 0, "BA", "BA", 0, 0},
    {"echo \\$HO‸", SC_ARG, SC_QN, "echo", 1, "\\$HO", "$HO", 0, 0},
    {"echo $_x9‸", SC_VAR, SC_QN, "echo", 1, "_x9", "_x9", 0, 0},
    {"echo \"${HO‸", SC_VAR, SC_QD, "echo", 1, "HO", "HO", SC_BRACE, 0},
    {"echo ${P‸", SC_VAR, SC_QN, "echo", 1, "P", "P", SC_BRACE, 0},
    {"x=\"${A‸", SC_VAR, SC_QD, NULL, 0, "A", "A", SC_BRACE, 0},
    {"echo pre${HO‸", SC_VAR, SC_QN, "echo", 1, "HO", "HO", SC_BRACE, 0},
    {"echo \"x ${HO‸", SC_VAR, SC_QD, "echo", 1, "HO", "HO", SC_BRACE, 0},
    {"echo \"$A$B‸", SC_VAR, SC_QD, "echo", 1, "B", "B", 0, 0},
    {"cat \"${‸", SC_VAR, SC_QD, "cat", 1, "", "", SC_BRACE, 0},
    {"echo $(echo $HO‸", SC_VAR, SC_QN, "echo", 1, "HO", "HO", 0, 1},

    /* -- single quotes: `$` is literal ------------------------- */
    {"cat 'my fi‸", SC_ARG, SC_QS, "cat", 1, "'my fi", "my fi", 0, 0},
    {"cat '‸", SC_ARG, SC_QS, "cat", 1, "'", "", 0, 0},
    {"echo 'a b‸", SC_ARG, SC_QS, "echo", 1, "'a b", "a b", 0, 0},
    {"echo 'it'\\''s‸", SC_ARG, SC_QS, "echo", 1, "'it'\\''s", "it's", 0,
     0},
    {"echo '${X‸", SC_ARG, SC_QS, "echo", 1, "'${X", "${X", 0, 0},
    {"echo '`x‸", SC_ARG, SC_QS, "echo", 1, "'`x", "`x", 0, 0},
    {"echo 'a|b‸", SC_ARG, SC_QS, "echo", 1, "'a|b", "a|b", 0, 0},
    {"echo '$(x‸", SC_ARG, SC_QS, "echo", 1, "'$(x", "$(x", 0, 0},
    {"cat 'unterminated‸' later", SC_ARG, SC_QS, "cat", 1, "'unterminated",
     "unterminated", 0, 0},
    {"cat 'a\nb‸", SC_ARG, SC_QS, "cat", 1, "'a\nb", "a\nb", 0, 0},

    /* -- double quotes, escapes, $'…' ------------------------- */
    {"cat \"‸", SC_ARG, SC_QD, "cat", 1, "\"", "", 0, 0},
    {"cat my\\ fi‸", SC_ARG, SC_QN, "cat", 1, "my\\ fi", "my fi", 0, 0},
    {"cat \"a\\\"b‸", SC_ARG, SC_QD, "cat", 1, "\"a\\\"b", "a\"b", 0, 0},
    {"cat \"a\"b‸", SC_ARG, SC_QN, "cat", 1, "\"a\"b", "ab", 0, 0},
    {"cat 'a'\"b‸", SC_ARG, SC_QD, "cat", 1, "'a'\"b", "ab", 0, 0},
    {"cat \"a\\nb‸", SC_ARG, SC_QD, "cat", 1, "\"a\\nb", "a\\nb", 0, 0},
    {"cat a\\‸", SC_ARG, SC_QN, "cat", 1, "a\\", "a", 0, 0},
    {"echo \"a b\" c‸", SC_ARG, SC_QN, "echo", 2, "c", "c", 0, 0},
    {"echo \"a‸ b\"", SC_ARG, SC_QD, "echo", 1, "\"a", "a", 0, 0},
    {"cat $'a\\tb‸", SC_ARG, SC_QL, "cat", 1, "$'a\\tb", "a\tb", 0, 0},
    {"cat $'\\x41‸", SC_ARG, SC_QL, "cat", 1, "$'\\x41", "A", 0, 0},
    {"cat $'it\\'s‸", SC_ARG, SC_QL, "cat", 1, "$'it\\'s", "it's", 0, 0},
    {"cat $'\\101‸", SC_ARG, SC_QL, "cat", 1, "$'\\101", "A", 0, 0},
    {"cat $'\\u00e9‸", SC_ARG, SC_QL, "cat", 1, "$'\\u00e9", "\xc3\xa9",
     0, 0},
    {"echo $'a‸", SC_ARG, SC_QL, "echo", 1, "$'a", "a", 0, 0},
    {"cat \"$(ls sr‸", SC_ARG, SC_QN, "ls", 1, "sr", "sr", 0, 1},
    {"cat \\\"x‸", SC_ARG, SC_QN, "cat", 1, "\\\"x", "\"x", 0, 0},

    /* -- `#` inside words and quotes is not a comment --------- */
    {"ls a#b‸", SC_ARG, SC_QN, "ls", 1, "a#b", "a#b", 0, 0},
    {"echo '#x‸", SC_ARG, SC_QS, "echo", 1, "'#x", "#x", 0, 0},
    {"echo \"#‸", SC_ARG, SC_QD, "echo", 1, "\"#", "#", 0, 0},
    {"echo a\\#b‸", SC_ARG, SC_QN, "echo", 1, "a\\#b", "a#b", 0, 0},
    {"echo x#‸", SC_ARG, SC_QN, "echo", 1, "x#", "x#", 0, 0},
    {"git log --grep \"#1‸", SC_ARG, SC_QD, "git", 3, "\"#1", "#1", 0, 0},
    {"echo $'#x‸", SC_ARG, SC_QL, "echo", 1, "$'#x", "#x", 0, 0},
    {"echo \"a\"#c‸", SC_ARG, SC_QN, "echo", 1, "\"a\"#c", "a#c", 0, 0},
    {"echo ${x}#y‸", SC_ARG, SC_QN, "echo", 1, "${x}#y", "#y", SC_EXPANDS,
     0},

    /* -- comments -------------------------------------------- */
    {"# comm‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"ls # c\ngi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"ls; #x‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"echo \"a\" #c‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"ls #‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"$(ls #x‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 1},
    {"ls #a\n#b\nc‸", SC_CMD, SC_QN, NULL, 0, "c", "c", 0, 0},
    {"echo x # 'y‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},

    /* -- redirections ---------------------------------------- */
    {"cat >> lo‸", SC_RED, SC_QN, "cat", 1, "lo", "lo", 0, 0},
    {"cat < in‸", SC_RED, SC_QN, "cat", 1, "in", "in", 0, 0},
    {"cmd &> al‸", SC_RED, SC_QN, "cmd", 1, "al", "al", 0, 0},
    {"cmd &>> al‸", SC_RED, SC_QN, "cmd", 1, "al", "al", 0, 0},
    {"cmd >| fo‸", SC_RED, SC_QN, "cmd", 1, "fo", "fo", 0, 0},
    {"cmd <> fo‸", SC_RED, SC_QN, "cmd", 1, "fo", "fo", 0, 0},
    {"cmd 2>&1 ar‸", SC_ARG, SC_QN, "cmd", 1, "ar", "ar", 0, 0},
    {"cmd >&2 ar‸", SC_ARG, SC_QN, "cmd", 1, "ar", "ar", 0, 0},
    {"cmd >&fi‸", SC_RED, SC_QN, "cmd", 1, "fi", "fi", 0, 0},
    {"cat > out ar‸", SC_ARG, SC_QN, "cat", 1, "ar", "ar", 0, 0},
    {"cat >out.txt a‸", SC_ARG, SC_QN, "cat", 1, "a", "a", 0, 0},
    {"> ou‸", SC_RED, SC_QN, "", 1, "ou", "ou", 0, 0},
    {"echo a2>fi‸", SC_RED, SC_QN, "echo", 2, "fi", "fi", 0, 0},
    {"cat >‸", SC_RED, SC_QN, "cat", 1, "", "", 0, 0},
    {"cat > \"my f‸", SC_RED, SC_QD, "cat", 1, "\"my f", "my f", 0, 0},
    {"sudo cat > ou‸", SC_RED, SC_QN, "cat", 1, "ou", "ou", 0, 0},
    {"cat 1>ou‸", SC_RED, SC_QN, "cat", 1, "ou", "ou", 0, 0},
    {"cat 2>>er‸", SC_RED, SC_QN, "cat", 1, "er", "er", 0, 0},
    {"cat 10>x‸", SC_RED, SC_QN, "cat", 1, "x", "x", 0, 0},
    {"cat 2> er‸", SC_RED, SC_QN, "cat", 1, "er", "er", 0, 0},
    {"cat 2<in‸", SC_RED, SC_QN, "cat", 1, "in", "in", 0, 0},
    {"echo 2 >x‸", SC_RED, SC_QN, "echo", 2, "x", "x", 0, 0},
    {"cat 2>&1 >ou‸", SC_RED, SC_QN, "cat", 1, "ou", "ou", 0, 0},
    {"cat 3< in‸", SC_RED, SC_QN, "cat", 1, "in", "in", 0, 0},
    {"cmd 2>&- ar‸", SC_ARG, SC_QN, "cmd", 1, "ar", "ar", 0, 0},

    /* -- here-strings and here-documents --------------------- */
    {"cat <<< wo‸", SC_ARG, SC_QN, "cat", 1, "wo", "wo", 0, 0},
    {"cat <<< $HO‸", SC_VAR, SC_QN, "cat", 1, "HO", "HO", 0, 0},
    {"cat <<-EO‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"cat <<'EO‸", SC_NON, SC_QS, NULL, 0, "", "", 0, 0},
    {"cat << EO‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"cat <<EOF\nbody‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"cat <<EOF\nbody\nEOF\nls | gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr",
     0, 0},
    {"cat <<EOF | gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0, 0},
    {"cat <<-EOF\n\tbody\n\tEOF\nx‸", SC_CMD, SC_QN, NULL, 0, "x", "x", 0,
     0},
    {"cat <<A <<B\na\nA\nb\nB\ngi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0,
     0},

    /* -- nesting --------------------------------------------- */
    {"echo `wh‸", SC_CMD, SC_QN, NULL, 0, "wh", "wh", 0, 1},
    {"echo `ls` fo‸", SC_ARG, SC_QN, "echo", 2, "fo", "fo", 0, 0},
    {"diff <(ls sr‸", SC_ARG, SC_QN, "ls", 1, "sr", "sr", 0, 1},
    {"tee >(gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0, 1},
    {"echo $(ls) fo‸", SC_ARG, SC_QN, "echo", 2, "fo", "fo", 0, 0},
    {"echo $(echo $(wh‸", SC_CMD, SC_QN, NULL, 0, "wh", "wh", 0, 2},
    {"(cd x) > ou‸", SC_RED, SC_QN, "", 1, "ou", "ou", 0, 0},
    {"echo \"$(wh‸", SC_CMD, SC_QN, NULL, 0, "wh", "wh", 0, 1},
    {"echo $(ls)x‸", SC_ARG, SC_QN, "echo", 1, "$(ls)x", "x", SC_EXPANDS,
     0},
    {"echo \"`wh‸", SC_CMD, SC_QN, NULL, 0, "wh", "wh", 0, 1},
    {"( ( gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 2},
    {"echo $(cat \"$(ls sr‸", SC_ARG, SC_QN, "ls", 1, "sr", "sr", 0, 2},
    {"echo $(ls \"my d‸", SC_ARG, SC_QD, "ls", 1, "\"my d", "my d", 0, 1},

    /* -- compound constructs ---------------------------------- */
    {"for f in *.c‸", SC_ARG, SC_QN, "", 1, "*.c", "*.c", 0, 0},
    {"for f in a b‸", SC_ARG, SC_QN, "", 2, "b", "b", 0, 0},
    {"for f‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"for f in a; do ec‸", SC_CMD, SC_QN, NULL, 0, "ec", "ec", 0, 0},
    {"select x in a‸", SC_ARG, SC_QN, "", 1, "a", "a", 0, 0},
    {"case $x in a) ec‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"case $x in a) b;; esac; gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0,
     0},
    {"case x‸", SC_ARG, SC_QN, "", 1, "x", "x", 0, 0},
    {"case x in (a) b;; esac | gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0,
     0},
    {"[[ -f fo‸", SC_ARG, SC_QN, "[[", 2, "fo", "fo", 0, 0},
    {"[[ a < b‸", SC_ARG, SC_QN, "[[", 3, "b", "b", 0, 0},
    {"[[ -f x ]] && gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"[[ a && b‸", SC_ARG, SC_QN, "[[", 3, "b", "b", 0, 0},
    {"(( i++ ‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"(( i++ )) && gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"echo $(( 1 + ‸", SC_NON, SC_QN, NULL, 0, "", "", 0, 0},
    {"echo $((1+2)) fo‸", SC_ARG, SC_QN, "echo", 2, "fo", "fo", 0, 0},
    {"if a; then b; fi > ou‸", SC_RED, SC_QN, "", 1, "ou", "ou", 0, 0},
    {"if a; then b; fi; gi‸", SC_CMD, SC_QN, NULL, 0, "gi", "gi", 0, 0},
    {"for x in 1; do y; done | gr‸", SC_CMD, SC_QN, NULL, 0, "gr", "gr", 0,
     0},
    {"f() { ec‸", SC_ARG, SC_QN, "f", 3, "ec", "ec", 0, 0},
    {"for ((i=0; i<3; i++)); do ec‸", SC_CMD, SC_QN, NULL, 0, "ec", "ec",
     0, 0},

    /* -- `--` ends options ------------------------------------ */
    {"rm -‸", SC_ARG, SC_QN, "rm", 1, "-", "-", 0, 0},
    {"rm '--' -r‸", SC_ARG, SC_QN, "rm", 2, "-r", "-r", 0, 0},
    {"git -- a -‸", SC_ARG, SC_QN, "git", 3, "-", "-", SC_DASHDASH, 0},
    {"ls -- a -b‸", SC_ARG, SC_QN, "ls", 3, "-b", "-b", SC_DASHDASH, 0},
    {"git checkout -- fi‸", SC_ARG, SC_QN, "git", 3, "fi", "fi",
     SC_DASHDASH, 0},
    {"rm -rf -- \"-x‸", SC_ARG, SC_QD, "rm", 3, "\"-x", "-x", SC_DASHDASH,
     0},
    {"sudo rm -- -x‸", SC_ARG, SC_QN, "rm", 2, "-x", "-x", SC_DASHDASH, 0},
    {"cmd --x -‸", SC_ARG, SC_QN, "cmd", 2, "-", "-", 0, 0},
    {"cmd a --‸", SC_ARG, SC_QN, "cmd", 2, "--", "--", 0, 0},
    {"cmd -- a b‸", SC_ARG, SC_QN, "cmd", 3, "b", "b", SC_DASHDASH, 0},
    {"cmd \\-- -x‸", SC_ARG, SC_QN, "cmd", 2, "-x", "-x", 0, 0},

    /* -- bytes: scanned, never decoded ----------------------- */
    {"cat \xff\xfe‸", SC_ARG, SC_QN, "cat", 1, "\xff\xfe", "\xff\xfe", 0,
     0},
    {"cat \xc3\xa9‸", SC_ARG, SC_QN, "cat", 1, "\xc3\xa9", "\xc3\xa9", 0,
     0},
    {"\xc3‸", SC_CMD, SC_QN, NULL, 0, "\xc3", "\xc3", 0, 0},
    {"echo \"\xff‸", SC_ARG, SC_QD, "echo", 1, "\"\xff", "\xff", 0, 0},
    {"cat \xe2\x80|x‸", SC_CMD, SC_QN, NULL, 0, "x", "x", 0, 0},
    {"ls \x7f‸", SC_ARG, SC_QN, "ls", 1, "\x7f", "\x7f", 0, 0},

    /* -- ordinary operands ------------------------------------ */
    {"ls ‸", SC_ARG, SC_QN, "ls", 1, "", "", 0, 0},
    {"ls src/ma‸", SC_ARG, SC_QN, "ls", 1, "src/ma", "src/ma", 0, 0},
    {"git commit -m x ‸", SC_ARG, SC_QN, "git", 4, "", "", 0, 0},
    {"cp a b c‸", SC_ARG, SC_QN, "cp", 3, "c", "c", 0, 0},
    {"ls   x‸", SC_ARG, SC_QN, "ls", 1, "x", "x", 0, 0},
    {"cd ~/‸", SC_ARG, SC_QN, "cd", 1, "~/", "~/", SC_TILDE, 0},
    {"cd ~ro‸", SC_ARG, SC_QN, "cd", 1, "~ro", "~ro", SC_TILDE, 0},
    {"cd \"~/‸", SC_ARG, SC_QD, "cd", 1, "\"~/", "~/", 0, 0},
    {"cd \\~/‸", SC_ARG, SC_QN, "cd", 1, "\\~/", "~/", 0, 0},
    {"echo a\\\nb‸", SC_ARG, SC_QN, "echo", 1, "a\\\nb", "ab", 0, 0},
    {"ls \\\n x‸", SC_ARG, SC_QN, "ls", 1, "x", "x", 0, 0}
};

/*
 * Sprint 57.32 §1: where the caret's command runs.  `cwd` is YewShCtx's
 * (relative to the `:!` directory, or absolute); NULL means UNKNOWN.
 * The environment is sh_cwd_env: HOME=/home/fix, OLDPWD=/prev/dir, no
 * CDPATH (CDPATH needs a tree on disk; test_shctx.c has those rows).
 * The first ten rows are fuzz seeds too (cd-00 .. cd-09).
 */
typedef struct ShCwdRow {
    const char *in;
    const char *cwd;
} ShCwdRow;

#define SC_UNKNOWN NULL

static const ShCwdRow sh_cwd_corpus[] = {
    /* -- the dogfood line -------------------------------------- */
    {"cd ch7/ && wolf build ou‸", "ch7"},
    /* -- §1 table 1: which commands move the shell ------------- */
    {"cd a && x‸", "a"},
    {"pushd a && x‸", "a"},
    {"cd -- a && x‸", "a"},
    {"cd && x‸", "/home/fix"},
    {"cd ~ && x‸", "/home/fix"},
    {"cd - && x‸", SC_UNKNOWN},
    {"popd && x‸", SC_UNKNOWN},
    {"pushd a && pushd b && popd && x‸", "a"},
    {"cd -P a && x‸", "a"},
    {"cd -L a && x‸", "a"},
    {"ls a && x‸", ""},
    /* -- §1 table 2: which connectors let a cd apply ----------- */
    {"cd a; x‸", "a"},
    {"cd a\nx‸", "a"},
    {"cd a || x‸", ""},
    {"cd a & x‸", ""},
    {"cd a | x‸", ""},
    {"(cd a; x‸", "a"},
    {"(cd a); x‸", ""},
    {"{ cd a; }; x‸", "a"},
    {"cd a && cd b && x‸", "a/b"},
    {"if cd a; then x‸", "a"},
    {"cd a || exit; x‸", "a"},
    {"cd a || return 1; x‸", "a"},
    {"cd a || echo no; x‸", SC_UNKNOWN},
    {"false || cd a && x‸", SC_UNKNOWN},
    /* -- the caret's own command ------------------------------- */
    {"cd ch‸", ""},
    {"cd a && cd b‸", "a"},
    {"cd a && cd ‸", "a"},
    /* -- operands ---------------------------------------------- */
    {"cd ~/proj && x‸", "/home/fix/proj"},
    {"cd $HOME && x‸", "/home/fix"},
    {"cd \"$HOME\"/w && x‸", "/home/fix/w"},
    {"cd ${HOME}/w && x‸", "/home/fix/w"},
    {"cd ${HOME}x && x‸", "/home/fixx"},
    {"cd $FOO && x‸", SC_UNKNOWN},
    {"cd $HOME$FOO && x‸", SC_UNKNOWN},
    {"cd $(pwd) && x‸", SC_UNKNOWN},
    {"cd `pwd` && x‸", SC_UNKNOWN},
    {"cd a* && x‸", SC_UNKNOWN},
    {"cd {a,b} && x‸", SC_UNKNOWN},
    {"cd .. && x‸", ".."},
    {"cd ../.. && cd a && x‸", "../../a"},
    {"cd a/../b/./c/ && x‸", "b/c"},
    {"cd a && cd .. && x‸", ""},
    {"cd \"my dir\" && x‸", "my dir"},
    {"cd 'q d'/e && x‸", "q d/e"},
    {"cd my\\ dir && x‸", "my dir"},
    {"cd /tmp && x‸", "/tmp"},
    {"cd /a/../.. && x‸", "/"},
    {"cd a && cd $PWD/b && x‸", "a/b"},
    {"cd $OLDPWD && x‸", "/prev/dir"},
    {"cd a && cd $OLDPWD && x‸", SC_UNKNOWN},
    {"cd \"~\" && x‸", "~"},
    {"cd ~nosuchuser-yew57 && x‸", SC_UNKNOWN},
    {"cd '' && x‸", SC_UNKNOWN},
    {"cd a b && x‸", SC_UNKNOWN},
    {"cd -e a && x‸", SC_UNKNOWN},
    {"FOO=1 cd a && x‸", SC_UNKNOWN},
    {"builtin cd a && x‸", "a"},
    {"command cd a && x‸", "a"},
    {"sudo cd a && x‸", ""},
    {"time cd a && x‸", "a"},
    {"HOME=/x; cd && x‸", SC_UNKNOWN},
    {"export HOME=/x; cd ~ && x‸", SC_UNKNOWN},
    /* -- pushd / popd ------------------------------------------ */
    {"pushd a && popd && x‸", ""},
    {"pushd a; pushd /t; popd; x‸", "a"},
    {"popd; pushd a; x‸", SC_UNKNOWN},
    {"pushd && x‸", SC_UNKNOWN},
    {"pushd +1 && x‸", SC_UNKNOWN},
    /* -- lists and pipelines ----------------------------------- */
    {"cd a &&\nx‸", "a"},
    {"cd a && x | y‸", "a"},
    {"cd a && x & y‸", ""},
    {"cd a; x & y‸", "a"},
    {"cd a | x; y‸", ""},
    {"x | cd a; y‸", SC_UNKNOWN},
    {"! cd a && x‸", SC_UNKNOWN},
    {"exit; x‸", SC_UNKNOWN},
    {"cd a; cd /abs; x‸", "/abs"},
    {"false || cd a; cd /t && x‸", "/t"},
    {"x && cd a; y‸", "a"},
    {"cd a || exit 2\ny‸", "a"},
    /* -- subshells, substitutions, groups ---------------------- */
    {"(cd a; (cd b; x‸", "a/b"},
    {"(cd a; (cd b); x‸", "a"},
    {"x $(cd a; y‸", "a"},
    {"echo $(cd a); x‸", ""},
    {"cd a; echo `cd b`; x‸", "a"},
    {"cd a && cat <(cd b; x‸", "a/b"},
    {"cd a && { cd b; x‸", "a/b"},
    {"{ cd a; } && x‸", "a"},
    {"{ cd a || exit; }; x‸", "a"},
    {"cd a || { echo no; exit 1; }; x‸", "a"},
    {"{ cd a; } | x; y‸", ""},
    /* -- compound commands ------------------------------------- */
    {"if cd a; then y; else x‸", ""},
    {"if cd a; then y; fi; x‸", SC_UNKNOWN},
    {"if true; then cd a; fi; x‸", SC_UNKNOWN},
    {"if true; then cd a; else cd a; fi; x‸", "a"},
    {"if cd a && cd b; then x‸", "a/b"},
    {"if x; then cd a; elif y; then z‸", ""},
    {"while cd a; do x‸", SC_UNKNOWN},
    {"until cd a; do x‸", ""},
    {"for f in 1 2; do x‸", ""},
    {"for f in 1 2; do cd a; x‸", SC_UNKNOWN},
    {"for f in 1 2; do (cd a; x‸", "a"},
    {"for f in 1; do cd a; done; x‸", SC_UNKNOWN},
    {"for f in 1; do ls; done; x‸", ""},
    {"case $x in a) cd b;; esac; x‸", SC_UNKNOWN},
    {"case $x in a) ls;; esac; x‸", ""},
    /* -- function definitions run nothing ---------------------- */
    {"f() { cd a; }; x‸", ""},
    {"f() { x‸", SC_UNKNOWN},
    {"f() { echo; cd /tmp; x‸", "/tmp"},
    /* 57.23's named limit: the body's first command is f's operands. */
    {"f() { cd /tmp; x‸", SC_UNKNOWN},
    {"f()\n{ cd a; }\nx‸", ""},
    {"function f { cd a; }; x‸", ""},
    {"f() (cd a); x‸", ""},
    {"f() { { x; }; cd a; }; y‸", ""}
};

#endif
