/* BSD file traversal and volume information cannot use Linux structures.
 * Paths name host files directly, just like open/stat and the native chooser. */
#include "internal.h"
#include "darwin_stat.h"
#include "machoman/host_filesystem.h"
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fts.h>
#include <memory>
#include <string>
#include <sys/vfs.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <unistd.h>

namespace {
struct darwin_ftsent {
    darwin_ftsent *cycle, *parent, *link;
    std::int64_t number;
    void *pointer;
    char *accpath, *path;
    std::int32_t error, symfd;
    std::uint16_t pathlen, namelen;
    std::uint64_t ino;
    std::int32_t dev;
    std::uint16_t nlink;
    std::int16_t level;
    std::uint16_t info, flags, instr;
    darwin_stat_t *statp;
    char name[1];
};
static_assert(offsetof(darwin_ftsent, info) == 88);
static_assert(offsetof(darwin_ftsent, statp) == 96);
static_assert(offsetof(darwin_ftsent, name) == 104);
struct entry_snapshot {
    std::unique_ptr<std::byte[]> bytes;
    darwin_stat_t stat{};
    std::string path, accpath;
    FTSENT *native;
    std::size_t capacity;
    explicit entry_snapshot(FTSENT *e) : bytes(new std::byte[sizeof(darwin_ftsent) + e->fts_namelen]{}), native(e), capacity(e->fts_namelen) {
        get()->number = e->fts_number;
        get()->pointer = e->fts_pointer;
    }
    darwin_ftsent *get() { return reinterpret_cast<darwin_ftsent *>(bytes.get()); }
};
/* The public prefix is the Darwin FTS layout. Most callers treat it as opaque,
 * but fts_cur and fts_options are documented and must also be translated. */
struct darwin_fts {
    darwin_ftsent *cur = nullptr, *child = nullptr, **array = nullptr;
    std::int32_t dev = 0;
    char *path = nullptr;
    std::int32_t rfd = -1, pathlen = 0, nitems = 0;
    const void *compar = nullptr;
    std::int32_t options = 0;
    FTS *native = nullptr;
    FTSENT *previous = nullptr;
    std::uint16_t previous_info = FTS_INIT;
    std::unordered_map<FTSENT *, std::unique_ptr<entry_snapshot>> entries;
    std::unordered_map<darwin_ftsent *, FTSENT *> reverse;
    std::unordered_map<FTSENT *, std::unordered_set<FTSENT *>> children;
    darwin_ftsent *entry(FTSENT *e) {
        if (!e) return nullptr;
        auto &slot = entries[e];
        if (slot && slot->capacity < e->fts_namelen) { reverse.erase(slot->get()); slot.reset(); }
        if (!slot) {
            slot = std::make_unique<entry_snapshot>(e);
            reverse[slot->get()] = e;
        }
        if (e->fts_parent) children[e->fts_parent].insert(e);
        auto *out = slot->get();
        /* libc's synthetic parent of a traversal root has no path/stat
         * payload. Its uninitialized fields must not be dereferenced. */
        if (e->fts_level == FTS_ROOTPARENTLEVEL) {
            out->level = FTS_ROOTPARENTLEVEL;
            out->info = e->fts_info;
            return out;
        }
        slot->path = e->fts_path ? e->fts_path : "";
        slot->accpath = e->fts_accpath ? e->fts_accpath : "";
        out->path = slot->path.data(); out->accpath = slot->accpath.data();
        out->error = mm_darwin_errno(e->fts_errno); out->symfd = e->fts_symfd;
        out->pathlen = e->fts_pathlen; out->namelen = e->fts_namelen;
        std::memcpy(out->name, e->fts_name, e->fts_namelen + 1);
        out->ino = e->fts_ino; out->dev = e->fts_dev; out->nlink = e->fts_nlink;
        out->level = e->fts_level; out->info = e->fts_info; out->flags = e->fts_flags; out->instr = e->fts_instr;
        out->statp = nullptr;
        if (e->fts_statp && e->fts_info != FTS_NS && e->fts_info != FTS_NSOK && e->fts_info != FTS_INIT) {
            convert_stat(*e->fts_statp, &slot->stat);
            out->statp = &slot->stat;
        }
        out->parent = entry(e->fts_parent);
        // libc only initializes fts_cycle for FTS_DC; other entries can carry
        // allocator bytes in that field.
        out->cycle = e->fts_info == FTS_DC && e->fts_cycle ? entry(e->fts_cycle) : nullptr;
        return out;
    }
    darwin_ftsent *list(FTSENT *head) {
        darwin_ftsent *first = nullptr, *last = nullptr;
        for (auto *e = head; e; e = e->fts_link) {
            auto *out = entry(e);
            if (last) last->link = out; else first = out;
            last = out;
        }
        if (last) last->link = nullptr;
        return first;
    }
    void forget(FTSENT *e) {
        if (auto it = children.find(e); it != children.end()) {
            auto descendants = std::move(it->second); children.erase(it);
            for (auto *child : descendants) forget(child);
        }
        if (auto it = entries.find(e); it != entries.end()) {
            auto *parent = it->second->get()->parent;
            if (auto found = reverse.find(parent); found != reverse.end()) {
                if (auto kids = children.find(found->second); kids != children.end()) kids->second.erase(e);
            }
            reverse.erase(it->second->get()); entries.erase(it);
        }
    }
};
static_assert(offsetof(darwin_fts, compar) == 56);
thread_local darwin_fts *sorting = nullptr;
struct sort_scope {
    darwin_fts *previous;
    explicit sort_scope(darwin_fts *s) : previous(std::exchange(sorting, s)) {}
    ~sort_scope() { sorting = previous; }
};
int compare_entries(const FTSENT **a, const FTSENT **b) {
    auto *left = sorting->entry(const_cast<FTSENT *>(*a));
    auto *right = sorting->entry(const_cast<FTSENT *>(*b));
    const std::uint64_t args[] = {reinterpret_cast<std::uintptr_t>(&left), reinterpret_cast<std::uintptr_t>(&right)};
    return static_cast<std::int32_t>(mm_call_callback(sorting->compar, args, 2));
}
struct darwin_statfs {
    std::uint32_t bsize; std::int32_t iosize;
    std::uint64_t blocks, bfree, bavail, files, ffree;
    std::int32_t fsid[2];
    std::uint32_t owner, type, flags, subtype;
    char typename_[16], mounted_on[1024], mounted_from[1024];
    std::uint32_t flags_ext, reserved[7];
};
static_assert(sizeof(darwin_statfs) == 2168);
void volume_info(const struct statfs &h, darwin_statfs *out, const char *path) {
    std::memset(out, 0, sizeof(*out));
    out->bsize = h.f_bsize; out->iosize = h.f_bsize;
    out->blocks = h.f_blocks; out->bfree = h.f_bfree; out->bavail = h.f_bavail;
    out->files = h.f_files; out->ffree = h.f_ffree;
    std::memcpy(out->fsid, &h.f_fsid, sizeof(out->fsid));
    out->owner = getuid(); out->type = h.f_type;
    auto mount = mmfs::mount_for(path);
    out->flags = mount.local ? 0x1000 : 0; /* MNT_LOCAL */
    if (h.f_flags & 1) out->flags |= 1; /* MNT_RDONLY */
    std::snprintf(out->typename_, sizeof(out->typename_), "%s", mount.type.c_str());
    std::snprintf(out->mounted_on, sizeof(out->mounted_on), "%s", mount.path.c_str());
    std::snprintf(out->mounted_from, sizeof(out->mounted_from), "%s", mount.source.c_str());
}
}
extern "C" {
darwin_fts *darwin_fts_open(char *const *paths, int options, const void *comparator) DARWIN(fts_open);
darwin_fts *darwin_fts_open(char *const *paths, int options, const void *comparator) {
    if (options & ~0xcff) { errno = EINVAL; return nullptr; }
    auto s = std::make_unique<darwin_fts>(); s->compar = comparator; s->options = options;
    sort_scope scope(s.get());
    // Avoid process-wide chdir from a scan worker. accpath is always usable.
    int flags = (options & 0x7f) | FTS_NOCHDIR;
    if (options & 0x400) flags |= FTS_COMFOLLOW;
    if (options & 0x800) flags |= FTS_NOSTAT;
    s->native = fts_open(paths, flags, comparator ? compare_entries : nullptr);
    if (!s->native) return nullptr;
    return s.release();
}
darwin_ftsent *darwin_fts_read(darwin_fts *s) DARWIN(fts_read);
darwin_ftsent *darwin_fts_read(darwin_fts *s) {
    sort_scope scope(s);
    auto *e = fts_read(s->native);
    // fts entries are invalidated on the next read, except directories which
    // remain parents until their postorder visit. Keep memory bounded by the
    // active directories and their unvisited children, not the whole scan.
    if (s->previous && s->previous != e && s->previous_info != FTS_D) s->forget(s->previous);
    s->previous = e;
    s->previous_info = e ? e->fts_info : FTS_INIT;
    s->cur = s->entry(e); s->child = nullptr;
    s->path = s->cur ? s->cur->path : nullptr;
    s->pathlen = s->cur ? s->cur->pathlen : 0;
    return s->cur;
}
darwin_ftsent *darwin_fts_children(darwin_fts *s, int options) DARWIN(fts_children);
darwin_ftsent *darwin_fts_children(darwin_fts *s, int options) {
    sort_scope scope(s);
    return s->child = s->list(fts_children(s->native, options));
}
int darwin_fts_set(darwin_fts *s, darwin_ftsent *e, int instruction) DARWIN(fts_set);
int darwin_fts_set(darwin_fts *s, darwin_ftsent *e, int instruction) {
    auto found = s->reverse.find(e);
    if (found == s->reverse.end()) { errno = EINVAL; return -1; }
    return fts_set(s->native, found->second, instruction);
}
int darwin_fts_close(darwin_fts *s) DARWIN(fts_close);
int darwin_fts_close(darwin_fts *s) { int result = fts_close(s->native); delete s; return result; }
int darwin_statfs(const char *path, struct darwin_statfs *out) DARWIN(statfs);
int darwin_statfs(const char *path, struct darwin_statfs *out) {
    struct statfs h; if (statfs(path, &h)) return -1; volume_info(h, out, path); return 0;
}
int darwin_fstatfs(int fd, struct darwin_statfs *out) DARWIN(fstatfs);
int darwin_fstatfs(int fd, struct darwin_statfs *out) {
    struct statfs h; if (fstatfs(fd, &h)) return -1;
    const auto path = std::filesystem::path("/proc/self/fd") / std::to_string(fd);
    volume_info(h, out, path.c_str()); return 0;
}
}
