# 1nsaneFont

1nsaneFont is **Mirabel Gothish** (Copyright 2026 Mirabel Project), adjusted to match the letters of
1NSANE (2000). The game keeps its letters as one small picture (`data\abc.pcx`, its shape in the alpha
picture `data\abc.acx`, each letter's box in `data\abc.pos`). Each character here was compared with that
picture and corrected where it differed. Mirabel Gothish carries no licence statement of its own; it is
included here with the repository owner's clearance for public release.

## What was changed

- **Spacing:** each character's advance is the width of its box in the game's picture, with no kerning,
  as the game spaces text. At 26 pixels per em, the font's letters fall where the game's do.
- **Letters:** `w` (pointed, two V's), `1` (a flag), `l` (a foot), `r` (a shorter arm), `R` and `k`
  (straight legs), `h` (a narrower right stem), `Q` (a shorter tail), `$` (a short centre line, set a
  little lower).
- **Symbols redrawn from the game's picture:** `@ # % ( ) [ ] / \ = + - _ ' , . : ~ * © ® |`. The game's
  apostrophe is a closing quote's shape (also at U+2019), and its `¿` sits on the baseline.
- **Accents:** one acute, grave, circumflex and diaeresis each, sized and placed as the game draws
  them; the tildes of Ñ and ñ as the game's.
- **Added:** ◀ ▶ ⏏ ■ (U+25C0, U+25B6, U+23CF, U+25A0), Ő ő Ű ű, and Ȕ ȕ (U and u with two slanted marks,
  which the game draws for two of its codes).

The rest of Mirabel Gothish's characters are unchanged, spaced as the game spaces its letters.

`MirabelGothish.otf` is the font as it was. Both are built into the program for the FONT option
(ORIGINAL, 1NSANEFONT, MIRABEL: runtime/font.c).

## The game's codes

The game's text is single-byte (Windows-1252). Most codes are their usual characters; these draw others:

| Code | Character | | Code | Character |
|---|---|---|---|---|
| `<` 0x3C | ◀ U+25C0 | | `ø` 0xF8 | ő U+0151 |
| `>` 0x3E | ▶ U+25B6 | | `ý` 0xFD | ű U+0171 |
| `^` 0x5E | ⏏ U+23CF | | `þ` 0xFE | Ű U+0170 |
| 0x7F | ■ U+25A0 | | `ÿ` 0xFF | Ő U+0150 |
| `œ` 0x9C | Ȕ U+0214 | | `÷` 0xF7 | ȕ U+0215 |

The game's Ê sits one pixel higher than its other letters (its record's offset is one off); here it
sits on the baseline.

## How close it is

Each of the game's 139 characters, drawn from this font at the game's size, differs from the game's
picture by 6.6% of its area on average (median 6.4%; 90% under 9.4%), mostly where the picture's
pixels blur the edges. Mirabel Gothish as it was differed by 24.1% on average, and lacked 10 of them.
