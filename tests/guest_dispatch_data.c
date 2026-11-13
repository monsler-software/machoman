#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "dispatch data: %s\n", message); exit(1); }
}
static void bytes_equal(dispatch_data_t data, const char *expected, size_t count) {
    const void *bytes = (void *)1;
    size_t size = SIZE_MAX;
    dispatch_data_t map = dispatch_data_create_map(data, &bytes, &size);
    check(map && size == count && dispatch_data_get_size(map) == count, "map size");
    check(!count || (bytes && memcmp(bytes, expected, count) == 0), "mapped content");
    dispatch_release(map);
}
static void drain(void) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false); }

int main(void) {
    char source[] = "abc";
    dispatch_data_t a = dispatch_data_create(source, 3, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    source[0] = 'x';
    bytes_equal(a, "abc", 3);
    char *owned = malloc(3);
    memcpy(owned, "DEF", 3);
    __block int destroyed = 0;
    dispatch_data_t b = dispatch_data_create(owned, 3, dispatch_get_main_queue(), ^{
        ++destroyed;
        free(owned);
    });
    dispatch_data_t combined = dispatch_data_create_concat(a, b);
    dispatch_release(a);
    dispatch_release(b);
    drain();
    check(destroyed == 0, "concat retains original storage");
    bytes_equal(combined, "abcDEF", 6);
    dispatch_data_t sub = dispatch_data_create_subrange(combined, 1, 4);
    bytes_equal(sub, "bcDE", 4);
    size_t offset = SIZE_MAX;
    dispatch_data_t region = dispatch_data_copy_region(sub, 2, &offset);
    check(offset == 2, "region offset in subrange");
    bytes_equal(region, "DE", 2);
    dispatch_release(region);
    __block unsigned visited = 0;
    __block dispatch_data_t retained_region = NULL;
    check(dispatch_data_apply(sub, ^bool(dispatch_data_t part, size_t position, const void *bytes, size_t size) {
        check(position == visited * 2 && size == 2, "region traversal offsets");
        check(memcmp(bytes, visited ? "DE" : "bc", 2) == 0, "region traversal bytes");
        if (!visited) { dispatch_retain(part); retained_region = part; }
        ++visited;
        return true;
    }), "complete traversal");
    check(visited == 2, "both regions traversed");
    visited = 0;
    check(!dispatch_data_apply(sub, ^bool(dispatch_data_t part, size_t position, const void *bytes, size_t size) {
        (void)part; (void)position; (void)bytes; (void)size;
        ++visited; return false;
    }) && visited == 1, "early traversal cancellation");
    dispatch_release(combined);
    drain();
    check(destroyed == 0, "subrange keeps custom storage");
    dispatch_release(sub);
    drain();
    check(destroyed == 1, "destructor dispatched exactly once on requested queue");
    bytes_equal(retained_region, "bc", 2);
    dispatch_release(retained_region);

    dispatch_data_t empty = dispatch_data_create("", 0, NULL, NULL);
    check(empty == dispatch_data_empty, "empty singleton export");
    bytes_equal(empty, "", 0);
    check(dispatch_data_apply(empty, ^bool(dispatch_data_t part, size_t pos, const void *bytes, size_t size) {
        (void)part; (void)pos; (void)bytes; (void)size;
        check(0, "empty must have no regions"); return false;
    }), "empty traversal succeeds");
    dispatch_retain(empty); dispatch_release(empty); dispatch_release(empty);
    a = dispatch_data_create("abc", 3, NULL, NULL);
    sub = dispatch_data_create_subrange(a, 1, SIZE_MAX);
    bytes_equal(sub, "bc", 2); dispatch_release(sub);
    sub = dispatch_data_create_subrange(a, SIZE_MAX, 4);
    check(sub == empty, "out of bounds subrange is empty"); dispatch_release(sub);
    region = dispatch_data_copy_region(a, 3, &offset);
    check(region == empty && offset == 3, "out of bounds region is empty"); dispatch_release(region);
    dispatch_release(a);

    owned = malloc(3); memcpy(owned, "xyz", 3);
    a = dispatch_data_create(owned, 3, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
    const void *mapped;
    size_t size;
    b = dispatch_data_create_map(a, &mapped, &size);
    check(mapped == owned && size == 3, "FREE uses original buffer");
    dispatch_release(a); bytes_equal(b, "xyz", 3); dispatch_release(b);
    size_t page = (size_t)getpagesize();
    void *memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    check(memory != MAP_FAILED, "allocate mmap backing");
    memcpy(memory, "map", 3);
    a = dispatch_data_create(memory, page, NULL, DISPATCH_DATA_DESTRUCTOR_MUNMAP);
    b = dispatch_data_create_subrange(a, 0, 3);
    dispatch_release(a); bytes_equal(b, "map", 3); dispatch_release(b);
    unsigned char residency;
    check(mincore(memory, page, (char *)&residency) != 0, "MUNMAP releases original mapping");
    puts("SDK dispatch data mapping, regions, ownership, blocks and destructors passed");
    return 0;
}
