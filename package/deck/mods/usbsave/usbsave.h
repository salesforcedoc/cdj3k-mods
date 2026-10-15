// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/usbsave.h - USB Save, the mod.
 *
 * When armed from the host, every track the deck loads is written out to
 * USB_SAVE/ on the first writable stick, losslessly, with the source file's own
 * tags. The two encoders (flac.c, wav.c) and the header reader (probe.c) are
 * pure and tested on a host; everything that touches a file, a thread or the
 * deck is in track_save.c.
 *
 * The arm flag is a FILE in /run, written by cdj3k-cfgd, because the mods run
 * inside EP122 and cannot open the cfg port that cfgd holds. See guest/cfgd.
 */
#ifndef EP122_MOD_USBSAVE_USBSAVE_H
#define EP122_MOD_USBSAVE_USBSAVE_H

/* Check the arm flag, and if a different track is loaded now, hand it to the
 * save thread. Cheap: the file is stat'd on a counter, not every call.
 *
 * [message] Called from the repaint tick in theme.c, next to the other reports,
 * because that is the one place guaranteed to run. It reads the loaded track's
 * path, which only the message thread may do. */
void mod_usbsave_pump(void);   /* [message] */

#endif /* EP122_MOD_USBSAVE_USBSAVE_H */
