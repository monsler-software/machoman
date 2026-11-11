#pragma once
#include <cstdint>
#include <cstring>
#include <sys/stat.h>

struct darwin_timespec { std::int64_t tv_sec, tv_nsec; };
struct darwin_stat_t {
    std::int32_t  st_dev;
    std::uint16_t st_mode;
    std::uint16_t st_nlink;
    std::uint64_t st_ino;
    std::uint32_t st_uid, st_gid;
    std::int32_t  st_rdev;
    std::int32_t  pad;
    darwin_timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    std::int64_t  st_size, st_blocks;
    std::int32_t  st_blksize;
    std::uint32_t st_flags, st_gen;
    std::int32_t  st_lspare;
    std::int64_t  st_qspare[2];
};
static_assert(sizeof(darwin_stat_t) == 144);

inline void convert_stat(const struct stat &h, darwin_stat_t *d) {
    std::memset(d, 0, sizeof *d);
    d->st_dev = static_cast<std::int32_t>(h.st_dev);
    d->st_mode = static_cast<std::uint16_t>(h.st_mode);
    d->st_nlink = static_cast<std::uint16_t>(h.st_nlink);
    d->st_ino = h.st_ino;
    d->st_uid = h.st_uid;
    d->st_gid = h.st_gid;
    d->st_rdev = static_cast<std::int32_t>(h.st_rdev);
    d->st_atimespec = {h.st_atim.tv_sec, h.st_atim.tv_nsec};
    d->st_mtimespec = {h.st_mtim.tv_sec, h.st_mtim.tv_nsec};
    d->st_ctimespec = {h.st_ctim.tv_sec, h.st_ctim.tv_nsec};
    d->st_birthtimespec = d->st_ctimespec;
    d->st_size = h.st_size;
    d->st_blocks = h.st_blocks;
    d->st_blksize = static_cast<std::int32_t>(h.st_blksize);
}

