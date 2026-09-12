// Minimal mmap/munmap/msync shim for the Nintendo Switch homebrew environment.
// See include/sys/mman.h for details and limitations.

#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

struct Mapping {
    size_t length; // user-visible length of the mapping
    int fd;        // backing file descriptor, or -1 for anonymous mappings
    int flags;     // mapping flags as passed to mmap()
    int prot;      // protection flags as passed to mmap()
};

Mapping *HeaderOf(void *addr) {
    return reinterpret_cast<Mapping *>(addr) - 1;
}

void *DataOf(Mapping *hdr) {
    return hdr + 1;
}

bool IsWritableShared(const Mapping &hdr) {
    return (hdr.flags & MAP_SHARED) != 0 && (hdr.prot & PROT_WRITE) != 0;
}

// Reads exactly `size` bytes from `fd` at the given offset into `dst`.
// Returns true on success.
bool ReadAllAt(int fd, off_t offset, void *dst, size_t size) {
    if (size == 0) {
        return true;
    }
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        return false;
    }
    char *out = static_cast<char *>(dst);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t got = read(fd, out, remaining);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            break; // short file: leave the rest zeroed
        }
        out += got;
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

// Writes `size` bytes from `src` to `fd` at the given offset. Returns true on success.
bool WriteAllAt(int fd, off_t offset, const void *src, size_t size) {
    if (size == 0) {
        return true;
    }
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        return false;
    }
    const char *in = static_cast<const char *>(src);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t put = write(fd, in, remaining);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        in += put;
        remaining -= static_cast<size_t>(put);
    }
    return true;
}

} // namespace

extern "C" {

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    Mapping *hdr = static_cast<Mapping *>(malloc(sizeof(Mapping) + length));
    if (hdr == nullptr) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    hdr->length = length;
    hdr->flags = flags;
    hdr->prot = prot;

    void *data = DataOf(hdr);
    memset(data, 0, length);

    const bool isAnonymous = (flags & MAP_ANONYMOUS) != 0 || fd == -1;
    if (isAnonymous) {
        hdr->fd = -1;
    } else {
        // File-backed mapping: duplicate the descriptor so the mapping owns it and
        // preload the file contents into the heap buffer.
        hdr->fd = dup(fd);
        if (hdr->fd == -1) {
            free(hdr);
            errno = EBADF;
            return MAP_FAILED;
        }
        if (!ReadAllAt(hdr->fd, offset, data, length)) {
            close(hdr->fd);
            free(hdr);
            errno = EIO;
            return MAP_FAILED;
        }
    }

    return data;
}

int munmap(void *addr, size_t length) {
    if (addr == nullptr) {
        errno = EINVAL;
        return -1;
    }
    Mapping *hdr = HeaderOf(addr);
    if (hdr->length != length) {
        errno = EINVAL;
        return -1;
    }
    // Best-effort write back for shared writable file mappings before releasing.
    if (IsWritableShared(*hdr) && hdr->fd != -1) {
        (void)WriteAllAt(hdr->fd, 0, DataOf(hdr), length);
    }
    if (hdr->fd != -1) {
        close(hdr->fd);
    }
    free(hdr);
    return 0;
}

int msync(void *addr, size_t length, int flags) {
    (void)flags;
    if (addr == nullptr) {
        errno = EINVAL;
        return -1;
    }
    Mapping *hdr = HeaderOf(addr);
    if (hdr->length != length) {
        errno = EINVAL;
        return -1;
    }
    if (IsWritableShared(*hdr) && hdr->fd != -1) {
        if (!WriteAllAt(hdr->fd, 0, DataOf(hdr), length)) {
            return -1;
        }
    }
    return 0;
}

} // extern "C"
