// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/key.c - the conversion described in key.h.
 *
 * The Camelot wheel: twelve positions, each holding a major key on its B side and
 * that key's relative minor on its A side. 8B is C major, and the numbers run from
 * there, so the major table below is indexed by pitch class (C=0, Db=1, D=2, ...)
 * and the minor side of the same position is always three steps back on the wheel.
 *
 * Enharmonic spellings collapse onto one pitch class, so C# and Db are the same
 * entry -- which is what makes "Db Minor" and "C# minor" both come out 12A.
 */
#include "usbsave/key.h"

#include <string.h>

/* Major-key Camelot number for each pitch class: C, Db, D, Eb, E, F, Gb, G, Ab, A,
 * Bb, B. (C=8B, Db=3B, D=10B, Eb=5B, E=12B, F=7B, Gb=2B, G=9B, Ab=4B, A=11B,
 * Bb=6B, B=1B -- the standard wheel.) */
static const int MAJOR_NUM[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };

static void skip_ws(const char *s, size_t *i)
{
    while (s[*i] == ' ' || s[*i] == '\t')
        (*i)++;
}

/* The pitch class of a note letter, or -1. 'B' and 'b' are both the note B; a flat
 * is read as the character AFTER a note, so the two never collide. */
static int note_pc(char c)
{
    switch (c) {
    case 'C': case 'c': return 0;
    case 'D': case 'd': return 2;
    case 'E': case 'e': return 4;
    case 'F': case 'f': return 5;
    case 'G': case 'g': return 7;
    case 'A': case 'a': return 9;
    case 'B': case 'b': return 11;
    default: return -1;
    }
}

/* Does `s` begin with `word`, ignoring case, without strncasecmp (not in the
 * mods' libc subset)? An empty `word` never matches. */
static int ci_prefix(const char *s, const char *word)
{
    size_t k = 0;

    for (; word[k] != '\0'; k++) {
        char c = s[k];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != word[k])
            return 0;
    }
    return k > 0;
}

static int emit(char *out, size_t cap, int num, char side)
{
    char tmp[4];
    int n = 0;

    if (num >= 10)
        tmp[n++] = (char)('0' + num / 10);
    tmp[n++] = (char)('0' + num % 10);
    tmp[n++] = side;
    if ((size_t)n + 1 > cap)
        return 0;
    memcpy(out, tmp, (size_t)n);
    out[n] = '\0';
    return 1;
}

/* A letter, for telling a bare quality initial from the head of a word: "m" is
 * minor, but "major" also starts with an m and is not. */
static int is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* A key already in Camelot: one or two digits, then A or B, then nothing. */
static int already_camelot(const char *s, size_t i, char *out, size_t cap)
{
    int n = 0, digits = 0;

    while (digits < 2 && s[i] >= '0' && s[i] <= '9') {
        n = n * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || n < 1 || n > 12)
        return 0;
    if (s[i] != 'A' && s[i] != 'a' && s[i] != 'B' && s[i] != 'b')
        return 0;
    {
        char side = (s[i] == 'A' || s[i] == 'a') ? 'A' : 'B';

        i++;
        skip_ws(s, &i);
        if (s[i] != '\0')
            return 0;
        return emit(out, cap, n, side);
    }
}

int mod_key_camelot(const char *key, char *out, size_t cap)
{
    size_t i = 0;
    int pc, num, minor = -1;

    if (!key || !out || cap == 0)
        return 0;
    skip_ws(key, &i);
    if (key[i] == '\0')
        return 0;
    if (already_camelot(key, i, out, cap))
        return 1;

    pc = note_pc(key[i]);
    if (pc < 0)
        return 0;
    i++;
    if (key[i] == '#') {
        pc = (pc + 1) % 12;
        i++;
    } else if (key[i] == 'b') {
        pc = (pc + 11) % 12;
        i++;
    }
    skip_ws(key, &i);

    /* The quality is what makes it decidable. "D" is D major or D minor and is
     * refused; "Dm", "D minor" and "D Major" are not. The words are tested before
     * the one-letter initials, because "major" begins with the m that means minor. */
    if (ci_prefix(key + i, "minor") || ci_prefix(key + i, "min"))
        minor = 1;
    else if (ci_prefix(key + i, "major") || ci_prefix(key + i, "maj"))
        minor = 0;
    else if (key[i] == 'm' && !is_alpha(key[i + 1]))
        minor = 1;
    else if (key[i] == 'M' && !is_alpha(key[i + 1]))
        minor = 0;
    if (minor < 0)
        return 0;

    if (minor) {
        num = MAJOR_NUM[pc] - 3;            /* the relative minor is three back */
        if (num <= 0)
            num += 12;
        return emit(out, cap, num, 'A');
    }
    return emit(out, cap, MAJOR_NUM[pc], 'B');
}
