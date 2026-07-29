#ifndef SJIS_H
#define SJIS_H

#include <tamtypes.h>

/* Shift-JIS to ASCII transliteration, shared by the PS2 (icon.sys) and PS1
   (title frame) save-title parsers - both store their title in Shift-JIS and
   both draw it with a Latin-only font.

   Takes one double-byte character and returns the closest ASCII stand-in, or
   '?' for kana and kanji, which have none. */
char sjis_wide_char(u16 code);

#endif
