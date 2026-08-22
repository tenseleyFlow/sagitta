#include "mod/git/fusscommit.h"

#include <string.h>

static bool fuss_commit_blank_byte(u8 byte)
{
    return byte == (u8)' ' || byte == (u8)'\t';
}

static bool fuss_commit_trailing_space(u8 byte)
{
    return fuss_commit_blank_byte(byte) || byte == (u8)'\r' ||
           byte == (u8)'\v' || byte == (u8)'\f';
}

static bool fuss_commit_line_starts_with(const u8 *message, size_t len,
                                         u8 candidate)
{
    size_t pos = 0U;

    while (pos < len) {
        size_t end = pos;
        size_t first;

        while (end < len && message[end] != (u8)'\n')
            end++;
        first = pos;
        while (first < end && fuss_commit_blank_byte(message[first]))
            first++;
        if (first < end && message[first] == candidate)
            return true;
        pos = end < len ? end + 1U : end;
    }
    return false;
}

bool yew_fuss_commit_select_comment(const u8 *message, size_t message_len,
                                    const u8 *setting, size_t setting_len,
                                    u8 *comment_char)
{
    static const u8 candidates[] = "#;@!$%^&|:";
    size_t i;

    if (comment_char == NULL || (message == NULL && message_len != 0U))
        return false;
    if (setting_len == 0U) {
        *comment_char = (u8)'#';
        return true;
    }
    if (setting == NULL)
        return false;
    if (setting_len == 1U) {
        if (setting[0] == 0U || setting[0] == (u8)'\n' ||
            fuss_commit_trailing_space(setting[0]))
            return false;
        *comment_char = setting[0];
        return true;
    }
    if (setting_len != 4U || memcmp(setting, "auto", 4U) != 0)
        return false;

    for (i = 0U; i < sizeof(candidates) - 1U; i++) {
        if (!fuss_commit_line_starts_with(message, message_len,
                                          candidates[i])) {
            *comment_char = candidates[i];
            return true;
        }
    }
    return false;
}

bool yew_fuss_commit_cleanup(Bytebuf *clean, const u8 *message,
                             size_t message_len, u8 comment_char)
{
    size_t pos = 0U;
    bool have_content = false;
    bool pending_blank = false;

    if (clean == NULL || (message == NULL && message_len != 0U))
        return false;
    clean->len = 0U;
    while (pos < message_len) {
        size_t end = pos;
        size_t first;
        size_t trimmed;

        while (end < message_len && message[end] != (u8)'\n')
            end++;
        first = pos;
        while (first < end && fuss_commit_blank_byte(message[first]))
            first++;
        if (first < end && message[first] == comment_char) {
            pos = end < message_len ? end + 1U : end;
            continue;
        }

        trimmed = end;
        while (trimmed > pos &&
               fuss_commit_trailing_space(message[trimmed - 1U]))
            trimmed--;
        if (trimmed == pos) {
            if (have_content)
                pending_blank = true;
        } else {
            if (have_content) {
                bytebuf_push_u8(clean, (u8)'\n');
                if (pending_blank)
                    bytebuf_push_u8(clean, (u8)'\n');
            }
            bytebuf_append(clean, message + pos, trimmed - pos);
            have_content = true;
            pending_blank = false;
        }
        pos = end < message_len ? end + 1U : end;
    }
    return !yew_fuss_commit_empty(clean->data, clean->len);
}

bool yew_fuss_commit_empty(const u8 *message, size_t message_len)
{
    (void)message;
    return message_len == 0U;
}
