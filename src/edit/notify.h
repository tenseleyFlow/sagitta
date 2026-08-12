#ifndef YEW_EDIT_NOTIFY_H
#define YEW_EDIT_NOTIFY_H

#include "text/edit.h"

void yew_edit_notify_pre(EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_edit_notify_post(EditCtx *ec, u8 kind, ByteOff at, u64 len);

#endif
