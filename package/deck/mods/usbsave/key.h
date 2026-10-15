// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/key.h - one notation for the key tag.
 *
 * The deck's own readout is inconsistent: some tracks it draws as Camelot ("12A",
 * "5B") and others as the musical key ("D", "Ebm"). Both are correct, but a tag
 * that mixes them is not a tag anyone can sort or filter on -- a key column wants
 * one alphabet.
 *
 * So everything goes to Camelot. When the cached metadata was found its Camelot
 * code is used directly; when it was not, the deck's readout is converted here.
 * The conversion REFUSES rather than guesses: "D" alone is D major or D minor and
 * there is no way to tell, so it is passed through as the deck wrote it rather than
 * being given a fifty-fifty answer.
 *
 * Pure: the mods' libc subset only -- no snprintf, no ctype, no strncasecmp.
 */
#ifndef EP122_MOD_USBSAVE_KEY_H
#define EP122_MOD_USBSAVE_KEY_H

#include <stddef.h>

/* The Camelot code for `key` in `out`, or 0 when it cannot be told or `out` is too
 * small. A key already in Camelot is normalised ("10b" -> "10B") and returned;
 * a musical key needs an explicit quality ("Ebm", "Eb Minor", "Eb Major") -- one
 * without it is ambiguous and refused. `out` is written only on success. */
int mod_key_camelot(const char *key, char *out, size_t cap);

#endif /* EP122_MOD_USBSAVE_KEY_H */
