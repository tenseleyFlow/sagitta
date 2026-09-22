#ifndef YEW_TEST_COMPSPEC_CORPUS_H
#define YEW_TEST_COMPSPEC_CORPUS_H

/*
 * Sprint 57.24 Testing Strategy: the spec resolution corpus.
 *
 * One row per case.  `in` is a `:!` body with the caret written as `‸`.
 * `kind` is yew_comp_shell_describe's answer for it: the sources the
 * SHELL dispatcher routes the caret's word to (`sub`, `flags`, `dash`,
 * `exec`, `path`, `path:lu`, `dir`, `var`, `values`, `gen:<name>`,
 * `none`, joined by `+`).  `rows`, when not NULL, is the candidate set
 * against the shipped specs in an empty workspace: space-separated and
 * EXACT, or, with a leading `~`, a set the answer must CONTAIN.
 */

typedef struct SpecCorpusRow {
    const char *in;
    const char *kind;
    const char *rows;
} SpecCorpusRow;

static const SpecCorpusRow spec_corpus[] = {
    /* --- wolf: the user's own language, `wolf build` above all --- */
    {"wolf bu\xe2\x80\xb8", "sub", "build"},
    {"wolf \xe2\x80\xb8", "sub", NULL},
    {"wolf build \xe2\x80\xb8", "path:lu", NULL},
    {"wolf build --emit=\xe2\x80\xb8", "values",
     "--emit=wir --emit=obj --emit=bin --emit=llvm-ir"},
    {"wolf build --emit=ll\xe2\x80\xb8", "values", "--emit=llvm-ir"},
    {"wolf build --emit \xe2\x80\xb8", "values", "wir obj bin llvm-ir"},
    {"wolf build -o \xe2\x80\xb8", "path", NULL},
    /* arg_optional: a following word is NOT the flag's value. */
    {"wolf build --profile-gen \xe2\x80\xb8", "path:lu", NULL},
    {"wolf build --profile-gen=\xe2\x80\xb8", "dir", NULL},
    {"wolf build --std-root \xe2\x80\xb8", "dir", NULL},
    {"wolf build --std-root=\xe2\x80\xb8", "dir", NULL},
    {"wolf build --profile=\xe2\x80\xb8", "path:wprof", NULL},
    {"wolf build -\xe2\x80\xb8", "flags", NULL},
    {"wolf build --rel\xe2\x80\xb8", "flags", "--release"},
    {"wolf build x.lu \xe2\x80\xb8", "none", NULL},
    {"wolf build --release \xe2\x80\xb8", "path:lu", NULL},
    {"wolf build --deny \xe2\x80\xb8", "values", "warnings"},
    {"wolf build --std-root /x \xe2\x80\xb8", "path:lu", NULL},
    {"wolf build --std-root ./\xe2\x80\xb8", "dir", NULL},
    {"wolf build ./sr\xe2\x80\xb8", "path:lu", NULL},
    {"wolf build ~/\xe2\x80\xb8", "path:lu", NULL},
    {"wolf build $(pwd)/x\xe2\x80\xb8", "none", NULL},
    {"wolf run x.lu \xe2\x80\xb8", "path", NULL},
    {"wolf run x.lu a b \xe2\x80\xb8", "path", NULL},
    {"wolf test \xe2\x80\xb8", "path:lu", NULL},
    {"wolf test a.lu b.lu \xe2\x80\xb8", "path:lu", NULL},
    {"wolf fmt --check \xe2\x80\xb8", "path:lu", NULL},
    {"wolf cache \xe2\x80\xb8", "sub", "path gc"},
    {"wolf cache gc --\xe2\x80\xb8", "flags", "--dry-run --all --help"},
    {"wolf profile merge \xe2\x80\xb8", "path:wprof", NULL},
    {"wolf add foo --path \xe2\x80\xb8", "dir", NULL},
    {"wolf add \xe2\x80\xb8", "none", NULL},
    {"wolf --completions \xe2\x80\xb8", "values", "bash zsh fish"},
    {"wolf --he\xe2\x80\xb8", "flags", "--help"},
    {"wolf build --he\xe2\x80\xb8", "flags", "--help"},
    {"wolf init --from-script \xe2\x80\xb8", "path:lu", NULL},
    {"wolf c-import -I \xe2\x80\xb8", "dir", NULL},
    {"wolf c-import \xe2\x80\xb8", "path:h", NULL},
    {"wolf lsp \xe2\x80\xb8", "none", NULL},
    {"wolf nosuch \xe2\x80\xb8", "none", NULL},
    {"/usr/local/bin/wolf bu\xe2\x80\xb8", "sub", "build"},
    {"./wolf bu\xe2\x80\xb8", "sub", "build"},
    {"wolf conform-run --error-format=\xe2\x80\xb8", "values",
     "--error-format=human --error-format=json"},
    {"echo x | wolf bu\xe2\x80\xb8", "sub", "build"},

    /* --- git --- */
    {"git remote a\xe2\x80\xb8", "sub", "add"},
    {"git remote \xe2\x80\xb8", "sub",
     "~add remove rename set-url show prune get-url"},
    {"git -C ../x che\xe2\x80\xb8", "sub", "~checkout"},
    {"git -C \xe2\x80\xb8", "dir", NULL},
    {"git -c \xe2\x80\xb8", "none", NULL},
    /* §3 pitfall: --since awaits a value, so -2w is it, not a flag. */
    {"git log --since -2w \xe2\x80\xb8", "gen:branches", NULL},
    {"git checkout -- \xe2\x80\xb8", "path", NULL},
    {"git checkout \xe2\x80\xb8", "gen:branches", NULL},
    {"git add ./sr\xe2\x80\xb8", "path", NULL},
    {"git add \xe2\x80\xb8", "gen:modified", NULL},
    {"git push \xe2\x80\xb8", "gen:remotes", NULL},
    {"git push origin \xe2\x80\xb8", "gen:branches", NULL},
    {"git remote remove \xe2\x80\xb8", "gen:remotes", NULL},
    {"git commit -m \xe2\x80\xb8", "none", NULL},
    {"git --no-pager lo\xe2\x80\xb8", "sub", "~log"},
    {"git \xe2\x80\xb8", "sub", "~add commit push pull remote"},
    {"git log $HO\xe2\x80\xb8", "var", NULL},
    {"sudo git che\xe2\x80\xb8", "sub", "~checkout"},
    {"sudo -u root git remote a\xe2\x80\xb8", "sub", "add"},

    /* --- tar: bundles --- */
    {"tar -xvf \xe2\x80\xb8", "path", NULL},
    {"tar -xvfz \xe2\x80\xb8", "path", NULL},
    {"tar -C \xe2\x80\xb8", "dir", NULL},
    {"tar -xv \xe2\x80\xb8", "path", NULL},
    {"tar --format=\xe2\x80\xb8", "values",
     "--format=ustar --format=pax --format=cpio --format=shar"},
    {"tar -cf out.tar \xe2\x80\xb8", "path", NULL},

    /* --- make: targets from the Makefile's text --- */
    {"make \xe2\x80\xb8", "gen:make_targets", NULL},
    {"make -C \xe2\x80\xb8", "dir", NULL},
    {"make -f \xe2\x80\xb8", "path", NULL},
    {"make -j \xe2\x80\xb8", "gen:make_targets", NULL},
    {"make -j4 \xe2\x80\xb8", "gen:make_targets", NULL},
    {"make install \xe2\x80\xb8", "gen:make_targets", NULL},
    {"make --directory=\xe2\x80\xb8", "dir", NULL},
    {"gmake \xe2\x80\xb8", "gen:make_targets", NULL},

    /* --- ssh / scp --- */
    {"ssh \xe2\x80\xb8", "gen:hosts", NULL},
    {"ssh -i \xe2\x80\xb8", "path", NULL},
    {"ssh -p \xe2\x80\xb8", "none", NULL},
    {"ssh -F \xe2\x80\xb8", "path", NULL},
    {"ssh -J \xe2\x80\xb8", "gen:hosts", NULL},
    {"ssh host \xe2\x80\xb8", "none", NULL},
    {"ssh -p 22 \xe2\x80\xb8", "gen:hosts", NULL},
    {"ssh -vp 22 \xe2\x80\xb8", "gen:hosts", NULL},
    {"ssh -O \xe2\x80\xb8", "values", "check forward cancel proxy exit stop"},
    {"scp -i \xe2\x80\xb8", "path", NULL},
    {"scp \xe2\x80\xb8", "path", NULL},

    /* --- kill: -SIGNAL spellings --- */
    {"kill -\xe2\x80\xb8", "flags+dash+gen:signals", "~-s -l -KILL -SIGTERM"},
    {"kill -KI\xe2\x80\xb8", "flags+dash+gen:signals", "-KILL"},
    {"kill -s \xe2\x80\xb8", "gen:signals", "~TERM SIGTERM HUP"},
    {"kill \xe2\x80\xb8", "gen:pids", NULL},
    {"kill -9 \xe2\x80\xb8", "gen:pids", NULL},

    /* --- precommands re-enter COMMAND position --- */
    {"sudo -u \xe2\x80\xb8", "gen:users", NULL},
    {"sudo -\xe2\x80\xb8", "flags", NULL},
    {"sudo \xe2\x80\xb8", "exec", NULL},
    {"sudo -u root \xe2\x80\xb8", "exec", NULL},
    {"sudo -T 5 wolf bu\xe2\x80\xb8", "sub", "build"},
    {"env FOO=1 \xe2\x80\xb8", "exec", NULL},
    {"env -u \xe2\x80\xb8", "var", NULL},
    {"nice -n 5 wolf bu\xe2\x80\xb8", "sub", "build"},
    {"timeout 5 \xe2\x80\xb8", "exec", NULL},
    {"timeout \xe2\x80\xb8", "none", NULL},
    {"timeout -s \xe2\x80\xb8", "gen:signals", NULL},
    {"xargs -I {} \xe2\x80\xb8", "exec", NULL},
    {"xargs -n 1 wolf bu\xe2\x80\xb8", "sub", "build"},
    {"nohup make \xe2\x80\xb8", "gen:make_targets", NULL},
    {"watch -n 2 \xe2\x80\xb8", "exec", NULL},
    {"watch -n 2 wolf bu\xe2\x80\xb8", "sub", "build"},
    {"doas -u \xe2\x80\xb8", "gen:users", NULL},

    /* --- the cd family: directories, past the first `/` too --- */
    {"cd \xe2\x80\xb8", "dir", NULL},
    {"cd src/\xe2\x80\xb8", "dir", NULL},
    {"pushd \xe2\x80\xb8", "dir", NULL},
    {"rmdir a \xe2\x80\xb8", "dir", NULL},
    {"mkdir -p \xe2\x80\xb8", "dir", NULL},
    {"mkdir -m \xe2\x80\xb8", "none", NULL},

    /* --- no spec: 57.23 exactly --- */
    {"ls \xe2\x80\xb8", "path", NULL},
    {"ls -\xe2\x80\xb8", "none", NULL},
    {"which \xe2\x80\xb8", "exec", NULL},
    {"echo $HO\xe2\x80\xb8", "var", NULL},
    {"wo\xe2\x80\xb8", "exec", NULL},
};

#endif
