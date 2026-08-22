#include "harness.h"
#include "mod/git/gutter.h"
#include <string.h>

void test_git_hunk_patch_headers_and_path_guard(void)
{
    Bytebuf b; GitHunk h={LINENO(0),LINENO(1),LINENO(0),LINENO(1),YEW_HUNK_MOD};
    YEW_ASSERT(yew_git_hunk_patch(&b,"file.c",(const u8*)"a\n",2,(const u8*)"b\n",2,&h));
    YEW_ASSERT(strstr((const char*)b.data,"diff --git a/file.c b/file.c")!=NULL);
    bytebuf_free(&b);
    YEW_ASSERT(!yew_git_hunk_patch(&b,"bad\nname",NULL,0,NULL,0,&h));
}
