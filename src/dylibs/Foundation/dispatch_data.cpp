/* Immutable dispatch data regions share backing storage. Subranges and
 * concatenations keep the original destructor alive until the last view
 * disappears; only mapping a fragmented object requires a byte copy. */
#include "foundation.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sys/mman.h>

namespace fnd {
FND_CLASS(OS_dispatch_data);

namespace {
enum class disposal { copied, free_buffer, unmap_buffer, block };
struct storage {
    const std::uint8_t *bytes = nullptr;
    std::size_t size = 0;
    disposal kind = disposal::copied;
    void *queue = nullptr;
    id destructor = nullptr;
    ~storage() {
        if (kind == disposal::copied || kind == disposal::free_buffer)
            std::free(const_cast<std::uint8_t *>(bytes));
        else if (kind == disposal::unmap_buffer) {
            if (bytes && size) munmap(const_cast<std::uint8_t *>(bytes), size);
        } else if (destructor) dispatch_submit_owned_block(queue, destructor);
    }
};
struct region {
    std::shared_ptr<storage> backing;
    std::size_t offset, size;
    const std::uint8_t *bytes() const { return backing->bytes + offset; }
};
struct data_object {
    mm_objc_object base;
    std::vector<region> regions;
    std::size_t size = 0;
};
Class data_class = nullptr;
char free_token, unmap_token;
data_object *new_data() { return make<data_object>(data_class); }
data_object *one_region(const region &r) {
    auto *result = new_data();
    result->regions.push_back(r);
    result->size = r.size;
    return result;
}
} // namespace
} // namespace fnd

using namespace fnd;
extern "C" {
EXPORT data_object dispatch_empty __asm__("__dispatch_data_empty");
EXPORT extern void *const dispatch_destructor_free __asm__("__dispatch_data_destructor_free") = &free_token;
EXPORT extern void *const dispatch_destructor_munmap __asm__("__dispatch_data_destructor_munmap") = &unmap_token;

data_object *darwin_dispatch_data_create(const void *buffer, std::size_t size, void *queue, void *destructor) DARWIN(dispatch_data_create);
data_object *darwin_dispatch_data_create(const void *buffer, std::size_t size, void *queue, void *destructor) {
    if (!buffer && size) return nullptr;
    auto owner = std::make_shared<storage>();
    owner->size = size;
    if (!destructor) {
        auto *copy = static_cast<std::uint8_t *>(std::malloc(size));
        if (!copy && size) return nullptr;
        if (size) std::memcpy(copy, buffer, size);
        owner->bytes = copy;
    } else {
        owner->bytes = static_cast<const std::uint8_t *>(buffer);
        if (destructor == dispatch_destructor_free) owner->kind = disposal::free_buffer;
        else if (destructor == dispatch_destructor_munmap) owner->kind = disposal::unmap_buffer;
        else {
            owner->kind = disposal::block;
            owner->queue = queue;
            owner->destructor = retain(static_cast<id>(destructor)); // Copies stack blocks.
        }
    }
    if (!size) return &dispatch_empty;
    return one_region({std::move(owner), 0, size});
}

std::size_t darwin_dispatch_data_get_size(data_object *data) DARWIN(dispatch_data_get_size);
std::size_t darwin_dispatch_data_get_size(data_object *data) { return data->size; }

data_object *darwin_dispatch_data_create_concat(data_object *a, data_object *b) DARWIN(dispatch_data_create_concat);
data_object *darwin_dispatch_data_create_concat(data_object *a, data_object *b) {
    if (!a->size) return reinterpret_cast<data_object *>(retain(reinterpret_cast<id>(b)));
    if (!b->size) return reinterpret_cast<data_object *>(retain(reinterpret_cast<id>(a)));
    if (b->size > std::numeric_limits<std::size_t>::max() - a->size) return nullptr;
    auto *result = new_data();
    result->regions = a->regions;
    result->regions.insert(result->regions.end(), b->regions.begin(), b->regions.end());
    result->size = a->size + b->size;
    return result;
}

data_object *darwin_dispatch_data_create_subrange(data_object *data, std::size_t offset, std::size_t length) DARWIN(dispatch_data_create_subrange);
data_object *darwin_dispatch_data_create_subrange(data_object *data, std::size_t offset, std::size_t length) {
    if (offset >= data->size || !length) return &dispatch_empty;
    length = std::min(length, data->size - offset);
    auto *result = new_data();
    result->size = length;
    for (const auto &r : data->regions) {
        if (offset >= r.size) { offset -= r.size; continue; }
        const auto count = std::min(length, r.size - offset);
        result->regions.push_back({r.backing, r.offset + offset, count});
        offset = 0;
        length -= count;
        if (!length) break;
    }
    return result;
}

data_object *darwin_dispatch_data_create_map(data_object *data, const void **buffer, std::size_t *size) DARWIN(dispatch_data_create_map);
data_object *darwin_dispatch_data_create_map(data_object *data, const void **buffer, std::size_t *size) {
    if (buffer) *buffer = nullptr;
    if (size) *size = 0;
    if (!data->size) return &dispatch_empty;
    data_object *result;
    if (data->regions.size() == 1) result = reinterpret_cast<data_object *>(retain(reinterpret_cast<id>(data)));
    else {
        auto owner = std::make_shared<storage>();
        auto *copy = static_cast<std::uint8_t *>(std::malloc(data->size));
        if (!copy) return nullptr;
        owner->bytes = copy;
        owner->size = data->size;
        std::size_t offset = 0;
        for (const auto &r : data->regions) { std::memcpy(copy + offset, r.bytes(), r.size); offset += r.size; }
        result = one_region({std::move(owner), 0, data->size});
    }
    if (buffer) *buffer = result->regions.front().bytes();
    if (size) *size = result->size;
    return result;
}

int darwin_dispatch_data_apply(data_object *data, void *applier) DARWIN(dispatch_data_apply);
int darwin_dispatch_data_apply(data_object *data, void *applier) {
    std::size_t offset = 0;
    for (const auto &r : data->regions) {
        auto *part = one_region(r);
        const std::uint64_t args[] = {ret(part), offset, ret(r.bytes()), r.size};
        const bool proceed = (mm_objc_block_invoke(applier, 4, args) & 0xff) != 0;
        release(reinterpret_cast<id>(part));
        if (!proceed) return 0;
        offset += r.size;
    }
    return 1;
}

data_object *darwin_dispatch_data_copy_region(data_object *data, std::size_t location, std::size_t *offset) DARWIN(dispatch_data_copy_region);
data_object *darwin_dispatch_data_copy_region(data_object *data, std::size_t location, std::size_t *offset) {
    std::size_t position = 0;
    for (const auto &r : data->regions) {
        if (location >= position && location - position < r.size) {
            *offset = position;
            return one_region(r);
        }
        position += r.size;
    }
    *offset = data->size;
    return &dispatch_empty;
}
} // extern "C"

namespace fnd {
void register_dispatch_data() {
    data_class = mm_objc_define_class("OS_dispatch_data", cls("NSObject"), sizeof(data_object),
        FND_CLASS_REF(OS_dispatch_data), FND_META_REF(OS_dispatch_data));
    register_lifecycle<data_object>(data_class);
    dispatch_empty.base.isa = data_class;
}
} // namespace fnd
