#pragma once

// Minimal <sys/mman.h> replacement for the Nintendo Switch (devkitA64 / newlib),
// which does not provide one.  Implemented on top of plain newlib file I/O and the
// heap so that code paths that only need memory-mapped *semantics* (mio backup RAM
// containers, anonymous mappings in util::VirtualMemory) compile and run.
//
// This is NOT a full POSIX mmap implementation: mappings are backed by heap buffers
// and file data is read/written in bulk. It is only intended for the files used by
// Ymir (small backup images) and for anonymous mappings.

#ifndef _SWITCH_SYS_MMAN_H
#define _SWITCH_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_FILE 0
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int msync(void *addr, size_t length, int flags);

#ifdef __cplusplus
}
#endif

#endif
