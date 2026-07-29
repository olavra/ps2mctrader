/*
 * PS2 Memory Card Trader
 *
 * Shift-JIS to ASCII transliteration for save titles - see sjis.h.
 */

#include "sjis.h"

/* JIS X 0208 row 1 (Shift-JIS lead byte 0x81) is punctuation and symbols, and
   a save title reaches for it constantly - the full-width colon in
   "Batman:Vengeance" and the dash in "MS Anthology - PROFILE" both live here,
   and without this table they came out as '?'. Indexed by the trail byte from
   0x40; '\0' means the character has no reasonable ASCII stand-in (kana
   repeat marks, math and shape symbols) and falls through to '?'. */
static const char row1[] = {
    /* 0x40 */ ' ', ',', '.', ',', '.', '.', ':', ';',
    /* 0x48 */ '?', '!', 0, 0, '\'', '`', '"', '^',
    /* 0x50 */ '~', '_', 0, 0, 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, '-', '-', '-', '/', '\\',
    /* 0x60 */ '~', '|', '|', '.', '.', '\'', '\'', '"',
    /* 0x68 */ '"', '(', ')', '[', ']', '[', ']', '{',
    /* 0x70 */ '}', '<', '>', '<', '>', '[', ']', '[',
    /* 0x78 */ ']', '[', ']', '+', '-', 0, 'x', 0,
    /* 0x80 */ 0, '=', 0, '<', '>', 0, 0, 0,
    /* 0x88 */ 0, 0, 0, 0, '\'', '"', 0, 0,
    /* 0x90 */ '$', 0, 0, '%', '#', '&', '*', '@',
    /* 0x98 */ 0, 0, 0, 0, 0, 0, 0, 0,
};

char sjis_wide_char(u16 code)
{
    /* Full-width digits and latin letters are contiguous ranges. */
    if (code >= 0x824F && code <= 0x8258) {
        return (char)('0' + (code - 0x824F));
    }
    if (code >= 0x8260 && code <= 0x8279) {
        return (char)('A' + (code - 0x8260));
    }
    if (code >= 0x8281 && code <= 0x829A) {
        return (char)('a' + (code - 0x8281));
    }

    if (code >= 0x8140 && code <= 0x819F) {
        char c = row1[(code & 0xFF) - 0x40];
        if (c != '\0') {
            return c;
        }
    }

    return '?';
}
