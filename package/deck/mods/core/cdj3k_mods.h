/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * cdj3k_mods.h - what a host shim links against.
 *
 * The mods install themselves from a constructor and register through the
 * `ep122_mods` linker section, so a host starts none of this. It only forwards
 * the file activity below, which the library watcher needs and only a syscall
 * interposer sees.
 */
#ifndef CDJ3K_MODS_H
#define CDJ3K_MODS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Forward from the host's open/write/close interposers. `ra` is the return
 * address of the caller that issued the write. */
void db_watch_open(const char *path, int fd, int flags);
void db_watch_write(int fd, size_t count, uintptr_t ra);
void db_watch_close(int fd);

/* Forward from the host's read interposer: one control-panel frame, in place.
 * A mod may set a switch bit or move the rotary counter in it, and whatever it
 * does not touch reaches EP122 exactly as the panel sent it -- which is the
 * whole of "simulate a key press", because the deck cannot tell a synthesised
 * frame from a real one. Called for every frame, so it must return immediately
 * when no mod has anything to say. */
void panel_frame(uint8_t *buf, size_t len);


#ifdef __cplusplus
}
#endif

#endif /* CDJ3K_MODS_H */
