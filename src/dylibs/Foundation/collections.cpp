/* NSArray, NSDictionary, NSSet, NSData, NSNumber, NSValue, NSNull and
 * their CF counterparts. Elements are retained; dictionary keys are
 * copied (NSCopying) and compared with -isEqual:/-hash so guest objects
 * work as keys. */
#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace fnd {

/* Class storage the guest binds to: external linkage, so not inside the
 * anonymous namespace below. */
FND_CLASS(NSArray); FND_CLASS(NSMutableArray); FND_CLASS(__NSArrayM); FND_CLASS(NSConstantArray);
FND_CLASS(NSDictionary); FND_CLASS(NSMutableDictionary); FND_CLASS(__NSDictionaryM);
FND_CLASS(NSSet); FND_CLASS(NSMutableSet); FND_CLASS(__NSSetM);
FND_CLASS(NSData); FND_CLASS(NSMutableData); FND_CLASS(__NSDataM);
FND_CLASS(NSValue); FND_CLASS(NSNumber); FND_CLASS(__NSCFNumber); FND_CLASS(__NSCFBoolean); FND_CLASS(NSConstantIntegerNumber);
FND_CLASS(NSNull);
FND_CLASS(NSConstantDoubleNumber);
FND_CLASS(NSEnumerator);

namespace {

struct array_object { mm_objc_object base; std::vector<id> items; bool mutable_ = false; };

struct key_hash { std::size_t operator()(id k) const { return static_cast<std::size_t>(object_hash(k)); } };
struct key_eq { bool operator()(id a, id b) const { return a == b || objects_equal(a, b); } };
struct dict_object {
    mm_objc_object base;
    std::unordered_map<id, id, key_hash, key_eq> map; /* key (copied, retained) -> value (retained) */
    std::vector<id> order;                            /* insertion order for enumeration */
    bool mutable_ = false;
};
struct set_object { mm_objc_object base; std::vector<id> items; bool mutable_ = false; };
struct data_object { mm_objc_object base; std::string bytes; bool mutable_ = false; };
struct number_object {
    mm_objc_object base;
    char type = 'q';          /* objc type code: c i s l q C I S L Q f d B */
    union { std::int64_t i; std::uint64_t u; double d; } v{};
};
struct value_object { mm_objc_object base; std::string bytes; std::string type; };


Class g_array, g_marray, g_carray, g_dict, g_mdict, g_cdict, g_set, g_mset, g_cset, g_data, g_mdata, g_cdata, g_value, g_number, g_cnumber, g_boolean, g_null, g_enumerator;
Class g_constant_double = nullptr;
struct constant_double { mm_objc_object base; double value; };
bool constant_double_number(id o) { return o && mm_objc_object_class(o)==g_constant_double; }

/* Exported singletons the guest binds to directly. */
extern "C" {
extern EXPORT number_object kCFBooleanTrue_storage __asm__("___kCFBooleanTrue");
extern EXPORT number_object kCFBooleanFalse_storage __asm__("___kCFBooleanFalse");
extern EXPORT id kCFBooleanTrue_ptr __asm__("_kCFBooleanTrue");
extern EXPORT id kCFBooleanFalse_ptr __asm__("_kCFBooleanFalse");
extern EXPORT array_object NSArray0_storage __asm__("___NSArray0__struct");
extern EXPORT dict_object NSDictionary0_storage __asm__("___NSDictionary0__struct");
extern EXPORT id kCFNull_ptr __asm__("_kCFNull");
/* CF callback tables: the guest passes pointers to these; we treat every
 * collection as retaining/copying, so their contents are never read. */
extern EXPORT std::uint64_t kCFTypeArrayCallBacks_[6] __asm__("_kCFTypeArrayCallBacks");
extern EXPORT std::uint64_t kCFTypeDictionaryKeyCallBacks_[7] __asm__("_kCFTypeDictionaryKeyCallBacks");
extern EXPORT std::uint64_t kCFCopyStringDictionaryKeyCallBacks_[7] __asm__("_kCFCopyStringDictionaryKeyCallBacks");
extern EXPORT std::uint64_t kCFTypeDictionaryValueCallBacks_[6] __asm__("_kCFTypeDictionaryValueCallBacks");
extern EXPORT std::uint64_t kCFTypeSetCallBacks_[6] __asm__("_kCFTypeSetCallBacks");
extern EXPORT std::uint64_t kCFCopyStringSetCallBacks_[6] __asm__("_kCFCopyStringSetCallBacks");
}
number_object kCFBooleanTrue_storage, kCFBooleanFalse_storage;
id kCFBooleanTrue_ptr, kCFBooleanFalse_ptr, kCFNull_ptr;
array_object NSArray0_storage;
dict_object NSDictionary0_storage;
std::uint64_t kCFTypeArrayCallBacks_[6], kCFTypeDictionaryKeyCallBacks_[7], kCFCopyStringDictionaryKeyCallBacks_[7],
    kCFTypeDictionaryValueCallBacks_[6], kCFTypeSetCallBacks_[6], kCFCopyStringSetCallBacks_[6];

/* New SDKs emit immutable NSArray literals in the image as isa, count,
 * items. Adapt that Darwin layout to the collection storage API without
 * writing into the image or treating it as a C++ vector. */
struct constant_array { mm_objc_object base; std::uint64_t count; id const *items; };
array_object *arr(id a) {
    if (mm_objc_object_class(a) != FND_CLASS_REF(NSConstantArray)) return as<array_object>(a);
    static std::mutex mutex;
    static std::unordered_map<id, std::unique_ptr<array_object>> cache;
    std::lock_guard lock(mutex);
    auto &storage = cache[a];
    if (!storage) {
        auto *constant = as<constant_array>(a);
        storage = std::make_unique<array_object>();
        storage->base.isa = g_array;
        if (constant->count) storage->items.assign(constant->items, constant->items + constant->count);
    }
    return storage.get();
}
dict_object *dict(id d) { return as<dict_object>(d); }

template <typename T> T *mutable_of(id o, const char *what) {
    auto *t = as<T>(o);
    if (!t->mutable_) { std::fprintf(stderr, "Foundation: %s sent to an immutable object\n", what); std::abort(); }
    return t;
}

id copy_key(id k) { return obj(send(k, "copyWithZone:", 0)); }

/* ---- NSArray ------------------------------------------------------------ */

std::uint64_t a_count(id self, SEL) { return arr(self)->items.size(); }
std::uint64_t a_objectAtIndex(id self, SEL, std::uint64_t i) {
    auto &v = arr(self)->items;
    if (i >= v.size()) { std::fprintf(stderr, "Foundation: -[NSArray objectAtIndex:] index %llu beyond bounds %zu\n", static_cast<unsigned long long>(i), v.size()); std::abort(); }
    return ret(v[i]);
}
std::uint64_t a_firstObject(id self, SEL) { auto &v = arr(self)->items; return v.empty() ? 0 : ret(v.front()); }
std::uint64_t a_lastObject(id self, SEL) { auto &v = arr(self)->items; return v.empty() ? 0 : ret(v.back()); }
std::uint64_t a_containsObject(id self, SEL, id o) { for (id e : arr(self)->items) if (objects_equal(e, o)) return 1; return 0; }
std::uint64_t a_indexOfObject(id self, SEL, id o) { auto &v = arr(self)->items; for (std::size_t i = 0; i < v.size(); i++) if (objects_equal(v[i], o)) return i; return NSNotFound; }
std::uint64_t a_indexOfObjectIdenticalTo(id self, SEL, id o) { auto &v = arr(self)->items; for (std::size_t i = 0; i < v.size(); i++) if (v[i] == o) return i; return NSNotFound; }
std::uint64_t a_getObjectsRange(id self, SEL, id *buf, std::uint64_t loc, std::uint64_t len) { auto &v = arr(self)->items; for (std::uint64_t i = 0; i < len && loc + i < v.size(); i++) buf[i] = v[loc + i]; return 0; }
std::uint64_t a_isEqualToArray(id self, SEL, id o) {
    if (!o || !is_array(o)) return 0;
    auto &a = arr(self)->items, &b = arr(o)->items;
    if (a.size() != b.size()) return 0;
    for (std::size_t i = 0; i < a.size(); i++) if (!objects_equal(a[i], b[i])) return 0;
    return 1;
}
std::uint64_t a_hash(id self, SEL) { return arr(self)->items.size(); }
std::uint64_t a_copy(id self, SEL) { if (!arr(self)->mutable_) return ret(retain(self)); return ret(array_create(arr(self)->items.data(), static_cast<CFIndex>(arr(self)->items.size()), false)); }
std::uint64_t a_copyWithZone(id self, SEL, void *) { return a_copy(self, nullptr); }
std::uint64_t a_mutableCopy(id self, SEL) { return ret(array_create(arr(self)->items.data(), static_cast<CFIndex>(arr(self)->items.size()), true)); }
std::uint64_t a_mutableCopyWithZone(id self, SEL, void *) { return a_mutableCopy(self, nullptr); }
std::uint64_t a_subarrayWithRange(id self, SEL, std::uint64_t loc, std::uint64_t len) {
    auto &v = arr(self)->items;
    if (loc > v.size()) loc = v.size();
    if (loc + len > v.size()) len = v.size() - loc;
    return ret(autorelease(array_create(v.data() + loc, static_cast<CFIndex>(len), false)));
}
std::uint64_t a_arrayByAddingObject(id self, SEL, id o) {
    std::vector<id> v = arr(self)->items; v.push_back(o);
    return ret(autorelease(array_create(v.data(), static_cast<CFIndex>(v.size()), false)));
}
std::uint64_t a_arrayByAddingObjectsFromArray(id self, SEL, id o) {
    std::vector<id> v = arr(self)->items; for (id e : arr(o)->items) v.push_back(e);
    return ret(autorelease(array_create(v.data(), static_cast<CFIndex>(v.size()), false)));
}
std::uint64_t a_componentsJoinedByString(id self, SEL, id sep) {
    std::u16string out; bool first = true;
    const chars s = string_chars(sep);
    for (id e : arr(self)->items) {
        if (!first) out.append(s.data, static_cast<std::size_t>(s.length));
        first = false;
        out += to_utf16(describe(e));
    }
    return ret(autorelease(string_from_utf16(out)));
}
std::uint64_t a_description(id self, SEL) {
    std::string s = "(\n";
    for (id e : arr(self)->items) { s += "    "; s += describe(e); s += ",\n"; }
    s += ")";
    return ret(autorelease(string_from_utf8(s)));
}
/* Fast enumeration: NSFastEnumerationState {state, itemsPtr, mutationsPtr, extra[5]}. */
struct fast_state { std::uint64_t state; id *items; std::uint64_t *mutations; std::uint64_t extra[5]; };
std::uint64_t a_countByEnumerating(id self, SEL, fast_state *st, id *buf, std::uint64_t len) {
    auto &v = arr(self)->items;
    if (st->state >= v.size()) return 0;
    const std::uint64_t n = std::min<std::uint64_t>(len, v.size() - st->state);
    for (std::uint64_t i = 0; i < n; i++) buf[i] = v[st->state + i];
    st->items = buf;
    st->mutations = &st->extra[0];
    st->state += n;
    return n;
}
std::uint64_t a_enumerateObjectsUsingBlock(id self, SEL, void *block) {
    auto v = arr(self)->items;
    std::uint8_t stop = 0;
    for (std::size_t i = 0; i < v.size() && !stop; i++) {
        const std::uint64_t args[3] = {ret(v[i]), i, ret(&stop)};
        mm_objc_block_invoke(block, 3, args);
    }
    return 0;
}
std::uint64_t a_sortedArrayUsingComparator(id self, SEL, void *block) {
    std::vector<id> v = arr(self)->items;
    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { const std::uint64_t args[2] = {ret(a), ret(b)}; return static_cast<std::int64_t>(mm_objc_block_invoke(block, 2, args)) < 0; });
    return ret(autorelease(array_create(v.data(), static_cast<CFIndex>(v.size()), false)));
}
std::uint64_t a_sortedArrayUsingSelector(id self, SEL, SEL cmp) {
    std::vector<id> v = arr(self)->items;
    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { return static_cast<std::int64_t>(mm_objc_send(a, mm_objc_sel_of(cmp), 1, reinterpret_cast<const std::uint64_t *>(&b))) < 0; });
    return ret(autorelease(array_create(v.data(), static_cast<CFIndex>(v.size()), false)));
}
std::uint64_t a_objectEnumerator(id self, SEL);
std::uint64_t a_reverseObjectEnumerator(id self, SEL);
std::uint64_t a_init(id self, SEL) { return ret(self); }
std::uint64_t a_initWithObjectsCount(id self, SEL, const id *objs, std::uint64_t n) { auto &v = arr(self)->items; for (std::uint64_t i = 0; i < n; i++) v.push_back(retain(objs[i])); return ret(self); }
std::uint64_t a_initWithArray(id self, SEL, id o) { auto &v = arr(self)->items; if (o) for (id e : arr(o)->items) v.push_back(retain(e)); return ret(self); }
std::uint64_t a_initWithArrayCopyItems(id self, SEL, id o, std::uint64_t copy) {
    auto &v = arr(self)->items;
    if (o) for (id e : arr(o)->items) v.push_back(copy ? copy_key(e) : retain(e));
    return ret(self);
}
/* initWithObjects: is variadic (nil-terminated), arguments on the guest stack after the first. */
std::uint64_t a_initWithObjects(id self, SEL, id first) {
    auto &v = arr(self)->items;
    if (!first) return ret(self);
    v.push_back(retain(first));
    const mm_cpu_context *ctx = api()->current_context();
    for (std::uint64_t p = ctx->x[31];; p += 8) {
        id e; std::memcpy(&e, reinterpret_cast<const void *>(p), 8);
        if (!e) break;
        v.push_back(retain(e));
    }
    return ret(self);
}
std::uint64_t a_initWithCapacity(id self, SEL, std::uint64_t n) { arr(self)->items.reserve(n); return ret(self); }
std::uint64_t a_initWithContentsOfFile(id self, SEL, id path) {
    FILE *f = std::fopen(to_utf8(path).c_str(), "rb");
    if (!f) { release(self); return 0; }
    std::string bytes; char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    id plist = plist_parse(bytes, nullptr);
    if (!plist || !is_array(plist)) { if (plist) release(plist); release(self); return 0; }
    for (id e : arr(plist)->items) arr(self)->items.push_back(retain(e));
    release(plist);
    return ret(self);
}
std::uint64_t a_writeToFileAtomically(id self, SEL, id path, std::uint64_t) {
    std::string out;
    if (!plist_write_xml(self, out)) return 0;
    FILE *f = std::fopen(to_utf8(path).c_str(), "wb");
    if (!f) return 0;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 1;
}

std::uint64_t ac_array(id c, SEL) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "init")))); }
std::uint64_t ac_arrayWithObject(id c, SEL, id o) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:count:", ret(&o), 1)))); }
std::uint64_t ac_arrayWithObjectsCount(id c, SEL, const id *objs, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:count:", ret(objs), n)))); }
std::uint64_t ac_arrayWithArray(id c, SEL, id o) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithArray:", ret(o))))); }
std::uint64_t ac_arrayWithObjects(id c, SEL, id first) {
    std::vector<id> v;
    if (first) {
        v.push_back(first);
        const mm_cpu_context *ctx = api()->current_context();
        for (std::uint64_t p = ctx->x[31];; p += 8) { id e; std::memcpy(&e, reinterpret_cast<const void *>(p), 8); if (!e) break; v.push_back(e); }
    }
    return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:count:", ret(v.data()), v.size()))));
}
std::uint64_t ac_arrayWithCapacity(id c, SEL, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithCapacity:", n)))); }
std::uint64_t ac_arrayWithContentsOfFile(id c, SEL, id path) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithContentsOfFile:", ret(path))))); }

/* NSMutableArray */
std::uint64_t ma_addObject(id self, SEL, id o) {
    if (!o) { std::fprintf(stderr, "Foundation: -[NSMutableArray addObject:] nil object\n"); std::abort(); }
    mutable_of<array_object>(self, "addObject:")->items.push_back(retain(o)); return 0;
}
std::uint64_t ma_addObjectsFromArray(id self, SEL, id o) { auto &v = mutable_of<array_object>(self, "addObjectsFromArray:")->items; if (o) for (id e : arr(o)->items) v.push_back(retain(e)); return 0; }
std::uint64_t ma_insertObjectAtIndex(id self, SEL, id o, std::uint64_t i) { auto &v = mutable_of<array_object>(self, "insertObject:atIndex:")->items; v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(i, v.size())), retain(o)); return 0; }
std::uint64_t ma_removeObjectAtIndex(id self, SEL, std::uint64_t i) {
    auto &v = mutable_of<array_object>(self, "removeObjectAtIndex:")->items;
    if (i >= v.size()) { std::fprintf(stderr, "Foundation: -[NSMutableArray removeObjectAtIndex:] index %llu beyond bounds\n", static_cast<unsigned long long>(i)); std::abort(); }
    id e = v[i]; v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); release(e); return 0;
}
std::uint64_t ma_removeLastObject(id self, SEL) { auto &v = mutable_of<array_object>(self, "removeLastObject")->items; if (!v.empty()) { id e = v.back(); v.pop_back(); release(e); } return 0; }
std::uint64_t ma_removeObject(id self, SEL, id o) {
    auto &v = mutable_of<array_object>(self, "removeObject:")->items;
    for (std::size_t i = v.size(); i-- > 0;) if (objects_equal(v[i], o)) { id e = v[i]; v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); release(e); }
    return 0;
}
std::uint64_t ma_removeObjectIdenticalTo(id self, SEL, id o) {
    auto &v = mutable_of<array_object>(self, "removeObjectIdenticalTo:")->items;
    for (std::size_t i = v.size(); i-- > 0;) if (v[i] == o) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); release(o); }
    return 0;
}
std::uint64_t ma_removeAllObjects(id self, SEL) { auto &v = mutable_of<array_object>(self, "removeAllObjects")->items; auto old = std::move(v); v.clear(); for (id e : old) release(e); return 0; }
std::uint64_t ma_removeObjectsInRange(id self, SEL, std::uint64_t loc, std::uint64_t len) {
    auto &v = mutable_of<array_object>(self, "removeObjectsInRange:")->items;
    if (loc > v.size()) return 0;
    if (loc + len > v.size()) len = v.size() - loc;
    std::vector<id> old(v.begin() + static_cast<std::ptrdiff_t>(loc), v.begin() + static_cast<std::ptrdiff_t>(loc + len));
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(loc), v.begin() + static_cast<std::ptrdiff_t>(loc + len));
    for (id e : old) release(e);
    return 0;
}
std::uint64_t ma_replaceObjectAtIndex(id self, SEL, std::uint64_t i, id o) { auto &v = mutable_of<array_object>(self, "replaceObjectAtIndex:withObject:")->items; if (i < v.size()) { id old = v[i]; v[i] = retain(o); release(old); } return 0; }
std::uint64_t ma_setArray(id self, SEL, id o) { ma_removeAllObjects(self, nullptr); return ma_addObjectsFromArray(self, nullptr, o); }
std::uint64_t ma_exchange(id self, SEL, std::uint64_t i, std::uint64_t j) { auto &v = mutable_of<array_object>(self, "exchangeObjectAtIndex:withObjectAtIndex:")->items; if (i < v.size() && j < v.size()) std::swap(v[i], v[j]); return 0; }
std::uint64_t ma_sortUsingComparator(id self, SEL, void *block) {
    auto &v = mutable_of<array_object>(self, "sortUsingComparator:")->items;
    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { const std::uint64_t args[2] = {ret(a), ret(b)}; return static_cast<std::int64_t>(mm_objc_block_invoke(block, 2, args)) < 0; });
    return 0;
}
std::uint64_t ma_sortUsingSelector(id self, SEL, SEL cmp) {
    auto &v = mutable_of<array_object>(self, "sortUsingSelector:")->items;
    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { return static_cast<std::int64_t>(mm_objc_send(a, mm_objc_sel_of(cmp), 1, reinterpret_cast<const std::uint64_t *>(&b))) < 0; });
    return 0;
}

/* ---- NSEnumerator (over a snapshot) -------------------------------------- */

struct enumerator_object { mm_objc_object base; std::vector<id> items; std::size_t next = 0; };
std::uint64_t e_nextObject(id self, SEL) { auto *e = as<enumerator_object>(self); return e->next < e->items.size() ? ret(e->items[e->next++]) : 0; }
std::uint64_t e_allObjects(id self, SEL) { auto *e = as<enumerator_object>(self); return ret(autorelease(array_create(e->items.data() + e->next, static_cast<CFIndex>(e->items.size() - e->next), false))); }
std::uint64_t e_countByEnumerating(id self, SEL, fast_state *st, id *buf, std::uint64_t len) {
    auto *e = as<enumerator_object>(self);
    if (e->next >= e->items.size()) return 0;
    const std::uint64_t n = std::min<std::uint64_t>(len, e->items.size() - e->next);
    for (std::uint64_t i = 0; i < n; i++) buf[i] = e->items[e->next + i];
    e->next += n;
    st->items = buf; st->mutations = &st->extra[0]; st->state = 1;
    return n;
}
id make_enumerator(std::vector<id> items) {
    auto *e = make<enumerator_object>(g_enumerator);
    for (id o : items) retain(o);
    e->items = std::move(items);
    return autorelease(reinterpret_cast<id>(e));
}
std::uint64_t a_objectEnumerator(id self, SEL) { return ret(make_enumerator(arr(self)->items)); }
std::uint64_t a_reverseObjectEnumerator(id self, SEL) { std::vector<id> v(arr(self)->items.rbegin(), arr(self)->items.rend()); return ret(make_enumerator(std::move(v))); }
struct enumerator_object_dtor : enumerator_object { ~enumerator_object_dtor() { for (id o : items) release(o); } };

/* ---- NSDictionary --------------------------------------------------------- */

std::uint64_t d_count(id self, SEL) { return dict(self)->map.size(); }
std::uint64_t d_objectForKey(id self, SEL, id k) { if (!k) return 0; auto &m = dict(self)->map; auto it = m.find(k); return it == m.end() ? 0 : ret(it->second); }
std::uint64_t d_allKeys(id self, SEL) { auto &o = dict(self)->order; return ret(autorelease(array_create(o.data(), static_cast<CFIndex>(o.size()), false))); }
std::uint64_t d_allValues(id self, SEL) {
    std::vector<id> v;
    for (id k : dict(self)->order) v.push_back(dict(self)->map[k]);
    return ret(autorelease(array_create(v.data(), static_cast<CFIndex>(v.size()), false)));
}
std::uint64_t d_keyEnumerator(id self, SEL) { return ret(make_enumerator(dict(self)->order)); }
std::uint64_t d_objectEnumerator(id self, SEL) { std::vector<id> v; for (id k : dict(self)->order) v.push_back(dict(self)->map[k]); return ret(make_enumerator(std::move(v))); }
std::uint64_t d_countByEnumerating(id self, SEL, fast_state *st, id *buf, std::uint64_t len) {
    auto &o = dict(self)->order;
    if (st->state >= o.size()) return 0;
    const std::uint64_t n = std::min<std::uint64_t>(len, o.size() - st->state);
    for (std::uint64_t i = 0; i < n; i++) buf[i] = o[st->state + i];
    st->items = buf; st->mutations = &st->extra[0]; st->state += n;
    return n;
}
std::uint64_t d_enumerateKeysAndObjects(id self, SEL, void *block) {
    auto order = dict(self)->order;
    std::uint8_t stop = 0;
    for (id k : order) {
        if (stop) break;
        auto it = dict(self)->map.find(k);
        if (it == dict(self)->map.end()) continue;
        const std::uint64_t args[3] = {ret(k), ret(it->second), ret(&stop)};
        mm_objc_block_invoke(block, 3, args);
    }
    return 0;
}
std::uint64_t d_isEqualToDictionary(id self, SEL, id o) {
    if (!o || !is_dictionary(o)) return 0;
    auto &a = dict(self)->map, &b = dict(o)->map;
    if (a.size() != b.size()) return 0;
    for (auto &[k, v] : a) { auto it = b.find(k); if (it == b.end() || !objects_equal(v, it->second)) return 0; }
    return 1;
}
std::uint64_t d_hash(id self, SEL) { return dict(self)->map.size(); }
id dict_copy(id self, bool mutable_) {
    id d = dictionary_create(mutable_);
    for (id k : dict(self)->order) dictionary_set(d, k, dict(self)->map[k]);
    return d;
}
std::uint64_t d_copy(id self, SEL) { if (!dict(self)->mutable_) return ret(retain(self)); return ret(dict_copy(self, false)); }
std::uint64_t d_copyWithZone(id self, SEL, void *) { return d_copy(self, nullptr); }
std::uint64_t d_mutableCopy(id self, SEL) { return ret(dict_copy(self, true)); }
std::uint64_t d_mutableCopyWithZone(id self, SEL, void *) { return d_mutableCopy(self, nullptr); }
std::uint64_t d_description(id self, SEL) {
    std::string s = "{\n";
    for (id k : dict(self)->order) { s += "    "; s += describe(k); s += " = "; s += describe(dict(self)->map[k]); s += ";\n"; }
    s += "}";
    return ret(autorelease(string_from_utf8(s)));
}
std::uint64_t d_init(id self, SEL) { return ret(self); }
std::uint64_t d_initWithObjectsForKeysCount(id self, SEL, const id *objs, const id *keys, std::uint64_t n) { for (std::uint64_t i = 0; i < n; i++) dictionary_set(self, keys[i], objs[i]); return ret(self); }
std::uint64_t d_initWithObjectsForKeys(id self, SEL, id objs, id keys) {
    auto &o = arr(objs)->items, &k = arr(keys)->items;
    for (std::size_t i = 0; i < o.size() && i < k.size(); i++) dictionary_set(self, k[i], o[i]);
    return ret(self);
}
std::uint64_t d_initWithDictionary(id self, SEL, id o) { if (o) for (id k : dict(o)->order) dictionary_set(self, k, dict(o)->map[k]); return ret(self); }
std::uint64_t d_initWithDictionaryCopyItems(id self, SEL, id o, std::uint64_t copy) {
    if (o) for (id k : dict(o)->order) { id v = dict(o)->map[k]; if (copy) { id c = copy_key(v); dictionary_set(self, k, c); release(c); } else dictionary_set(self, k, v); }
    return ret(self);
}
/* initWithObjectsAndKeys: v1, k1, v2, k2, ..., nil */
std::uint64_t d_initWithObjectsAndKeys(id self, SEL, id first) {
    if (!first) return ret(self);
    const mm_cpu_context *ctx = api()->current_context();
    std::uint64_t p = ctx->x[31];
    id value = first;
    while (true) {
        id key; std::memcpy(&key, reinterpret_cast<const void *>(p), 8); p += 8;
        if (!key) break;
        dictionary_set(self, key, value);
        std::memcpy(&value, reinterpret_cast<const void *>(p), 8); p += 8;
        if (!value) break;
    }
    return ret(self);
}
std::uint64_t d_initWithCapacity(id self, SEL, std::uint64_t) { return ret(self); }
std::uint64_t d_initWithContentsOfFile(id self, SEL, id path) {
    FILE *f = std::fopen(to_utf8(path).c_str(), "rb");
    if (!f) { release(self); return 0; }
    std::string bytes; char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    id plist = plist_parse(bytes, nullptr);
    if (!plist || !is_dictionary(plist)) { if (plist) release(plist); release(self); return 0; }
    for (id k : dict(plist)->order) dictionary_set(self, k, dict(plist)->map[k]);
    release(plist);
    return ret(self);
}
std::uint64_t d_writeToFileAtomically(id self, SEL, id path, std::uint64_t) {
    std::string out;
    if (!plist_write_xml(self, out)) return 0;
    FILE *f = std::fopen(to_utf8(path).c_str(), "wb");
    if (!f) return 0;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 1;
}
std::uint64_t d_valueForKey(id self, SEL, id k) { return d_objectForKey(self, nullptr, k); }
std::uint64_t d_objectForKeyedSubscript(id self, SEL, id k) { return d_objectForKey(self, nullptr, k); }

std::uint64_t dc_dictionary(id c, SEL) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "init")))); }
std::uint64_t dc_dictionaryWithObjectForKey(id c, SEL, id o, id k) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:forKeys:count:", ret(&o), ret(&k), 1)))); }
std::uint64_t dc_dictionaryWithObjectsForKeysCount(id c, SEL, const id *o, const id *k, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:forKeys:count:", ret(o), ret(k), n)))); }
std::uint64_t dc_dictionaryWithObjectsForKeys(id c, SEL, id o, id k) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:forKeys:", ret(o), ret(k))))); }
std::uint64_t dc_dictionaryWithDictionary(id c, SEL, id d) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithDictionary:", ret(d))))); }
std::uint64_t dc_dictionaryWithObjectsAndKeys(id c, SEL, id first) {
    id d = obj(send(c, "alloc"));
    if (first) {
        const mm_cpu_context *ctx = api()->current_context();
        std::uint64_t p = ctx->x[31];
        id value = first;
        while (true) {
            id key; std::memcpy(&key, reinterpret_cast<const void *>(p), 8); p += 8;
            if (!key) break;
            dictionary_set(d, key, value);
            std::memcpy(&value, reinterpret_cast<const void *>(p), 8); p += 8;
            if (!value) break;
        }
    }
    return ret(autorelease(d));
}
std::uint64_t dc_dictionaryWithCapacity(id c, SEL, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithCapacity:", n)))); }
std::uint64_t dc_dictionaryWithContentsOfFile(id c, SEL, id path) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithContentsOfFile:", ret(path))))); }

/* NSMutableDictionary */
std::uint64_t md_setObjectForKey(id self, SEL, id o, id k) {
    if (!o || !k) { std::fprintf(stderr, "Foundation: -[NSMutableDictionary setObject:forKey:] nil %s\n", o ? "key" : "object"); std::abort(); }
    mutable_of<dict_object>(self, "setObject:forKey:"); dictionary_set(self, k, o); return 0;
}
std::uint64_t md_setObjectForKeyedSubscript(id self, SEL, id o, id k) { mutable_of<dict_object>(self, "setObject:forKeyedSubscript:"); if (o) dictionary_set(self, k, o); else dictionary_remove(self, k); return 0; }
std::uint64_t md_setValueForKey(id self, SEL, id o, id k) { return md_setObjectForKeyedSubscript(self, nullptr, o, k); }
std::uint64_t md_removeObjectForKey(id self, SEL, id k) { mutable_of<dict_object>(self, "removeObjectForKey:"); dictionary_remove(self, k); return 0; }
std::uint64_t md_removeAllObjects(id self, SEL) { auto keys = mutable_of<dict_object>(self, "removeAllObjects")->order; for (id k : keys) dictionary_remove(self, k); return 0; }
std::uint64_t md_addEntriesFromDictionary(id self, SEL, id o) { mutable_of<dict_object>(self, "addEntriesFromDictionary:"); if (o) for (id k : dict(o)->order) dictionary_set(self, k, dict(o)->map[k]); return 0; }
std::uint64_t md_setDictionary(id self, SEL, id o) { md_removeAllObjects(self, nullptr); return md_addEntriesFromDictionary(self, nullptr, o); }

/* ---- NSSet ---------------------------------------------------------------- */

set_object *set(id s) { return as<set_object>(s); }
bool set_contains(set_object *s, id o) { for (id e : s->items) if (objects_equal(e, o)) return true; return false; }
std::uint64_t s_count(id self, SEL) { return set(self)->items.size(); }
std::uint64_t s_containsObject(id self, SEL, id o) { return set_contains(set(self), o); }
std::uint64_t s_member(id self, SEL, id o) { for (id e : set(self)->items) if (objects_equal(e, o)) return ret(e); return 0; }
std::uint64_t s_allObjects(id self, SEL) { auto &v = set(self)->items; return ret(autorelease(array_create(v.data(), static_cast<CFIndex>(v.size()), false))); }
std::uint64_t s_anyObject(id self, SEL) { auto &v = set(self)->items; return v.empty() ? 0 : ret(v.front()); }
std::uint64_t s_objectEnumerator(id self, SEL) { return ret(make_enumerator(set(self)->items)); }
std::uint64_t s_countByEnumerating(id self, SEL, fast_state *st, id *buf, std::uint64_t len) {
    auto &v = set(self)->items;
    if (st->state >= v.size()) return 0;
    const std::uint64_t n = std::min<std::uint64_t>(len, v.size() - st->state);
    for (std::uint64_t i = 0; i < n; i++) buf[i] = v[st->state + i];
    st->items = buf; st->mutations = &st->extra[0]; st->state += n;
    return n;
}
std::uint64_t s_init(id self, SEL) { return ret(self); }
std::uint64_t s_initWithCapacity(id self, SEL, std::uint64_t capacity) { set(self)->items.reserve(capacity); return ret(self); }
std::uint64_t s_initWithObjectsCount(id self, SEL, const id *objs, std::uint64_t n) { for (std::uint64_t i = 0; i < n; i++) if (!set_contains(set(self), objs[i])) set(self)->items.push_back(retain(objs[i])); return ret(self); }
std::uint64_t s_initWithArray(id self, SEL, id a) { if (a) for (id e : arr(a)->items) if (!set_contains(set(self), e)) set(self)->items.push_back(retain(e)); return ret(self); }
std::uint64_t s_initWithSet(id self, SEL, id a) { if (a) for (id e : set(a)->items) set(self)->items.push_back(retain(e)); return ret(self); }
std::uint64_t s_initWithObjects(id self, SEL, id first) {
    if (!first) return ret(self);
    std::vector<id> v{first};
    const mm_cpu_context *ctx = api()->current_context();
    for (std::uint64_t p = ctx->x[31];; p += 8) { id e; std::memcpy(&e, reinterpret_cast<const void *>(p), 8); if (!e) break; v.push_back(e); }
    return s_initWithObjectsCount(self, nullptr, v.data(), v.size());
}
std::uint64_t s_copy(id self, SEL) { if (!set(self)->mutable_) return ret(retain(self)); id s = obj(send(obj(send(reinterpret_cast<id>(g_cset), "alloc")), "initWithSet:", ret(self))); return ret(s); }
std::uint64_t s_copyWithZone(id self, SEL, void *) { return s_copy(self, nullptr); }
std::uint64_t s_mutableCopy(id self, SEL) { id s = obj(send(obj(send(reinterpret_cast<id>(g_cset), "alloc")), "initWithSet:", ret(self))); set(s)->mutable_ = true; return ret(s); }
std::uint64_t s_mutableCopyWithZone(id self, SEL, void *) { return s_mutableCopy(self, nullptr); }
std::uint64_t s_isEqualToSet(id self, SEL, id o) {
    if (!o || !is_kind(o, g_set) || set(self)->items.size() != set(o)->items.size()) return 0;
    for (id e : set(self)->items) if (!set_contains(set(o), e)) return 0;
    return 1;
}
std::uint64_t sc_set(id c, SEL) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "init")))); }
std::uint64_t sc_setWithCapacity(id c, SEL, std::uint64_t capacity) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithCapacity:", capacity)))); }
std::uint64_t sc_setWithObject(id c, SEL, id o) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:count:", ret(&o), 1)))); }
std::uint64_t sc_setWithArray(id c, SEL, id a) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithArray:", ret(a))))); }
std::uint64_t sc_setWithObjects(id c, SEL, id first) {
    std::vector<id> v;
    if (first) {
        v.push_back(first);
        const mm_cpu_context *ctx = api()->current_context();
        for (std::uint64_t p = ctx->x[31];; p += 8) { id e; std::memcpy(&e, reinterpret_cast<const void *>(p), 8); if (!e) break; v.push_back(e); }
    }
    return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithObjects:count:", ret(v.data()), v.size()))));
}
std::uint64_t ms_addObject(id self, SEL, id o) { auto *s = mutable_of<set_object>(self, "addObject:"); if (o && !set_contains(s, o)) s->items.push_back(retain(o)); return 0; }
std::uint64_t ms_removeObject(id self, SEL, id o) {
    auto *s = mutable_of<set_object>(self, "removeObject:");
    for (std::size_t i = 0; i < s->items.size(); i++) if (objects_equal(s->items[i], o)) { id e = s->items[i]; s->items.erase(s->items.begin() + static_cast<std::ptrdiff_t>(i)); release(e); break; }
    return 0;
}
std::uint64_t ms_removeAllObjects(id self, SEL) { auto *s = mutable_of<set_object>(self, "removeAllObjects"); auto old = std::move(s->items); s->items.clear(); for (id e : old) release(e); return 0; }
std::uint64_t ms_addObjectsFromArray(id self, SEL, id a) { for (id e : arr(a)->items) ms_addObject(self, nullptr, e); return 0; }
std::uint64_t ms_unionSet(id self, SEL, id o) { for (id e : set(o)->items) ms_addObject(self, nullptr, e); return 0; }
std::uint64_t ms_minusSet(id self, SEL, id o) { for (id e : set(o)->items) ms_removeObject(self, nullptr, e); return 0; }

/* ---- NSData ----------------------------------------------------------------- */

data_object *dat(id d) { return as<data_object>(d); }
std::uint64_t n_length(id self, SEL) { return dat(self)->bytes.size(); }
std::uint64_t n_bytes(id self, SEL) { return ret(dat(self)->bytes.data()); }
std::uint64_t n_mutableBytes(id self, SEL) { return ret(mutable_of<data_object>(self, "mutableBytes")->bytes.data()); }
std::uint64_t n_getBytesLength(id self, SEL, void *buf, std::uint64_t len) { auto &b = dat(self)->bytes; std::memcpy(buf, b.data(), std::min<std::size_t>(len, b.size())); return 0; }
std::uint64_t n_getBytesRange(id self, SEL, void *buf, std::uint64_t loc, std::uint64_t len) { auto &b = dat(self)->bytes; if (loc < b.size()) std::memcpy(buf, b.data() + loc, std::min<std::size_t>(len, b.size() - loc)); return 0; }
std::uint64_t n_isEqualToData(id self, SEL, id o) { return o && is_data(o) && dat(self)->bytes == dat(o)->bytes; }
std::uint64_t n_hash(id self, SEL) { std::uint64_t h = 1469598103934665603ull; for (unsigned char c : dat(self)->bytes) { h ^= c; h *= 1099511628211ull; } return h; }
std::uint64_t n_subdataWithRange(id self, SEL, std::uint64_t loc, std::uint64_t len) { auto &b = dat(self)->bytes; if (loc > b.size()) loc = b.size(); return ret(autorelease(data_create(b.data() + loc, static_cast<CFIndex>(std::min<std::size_t>(len, b.size() - loc)), false))); }
std::uint64_t n_copy(id self, SEL) { if (!dat(self)->mutable_) return ret(retain(self)); return ret(data_create(dat(self)->bytes.data(), static_cast<CFIndex>(dat(self)->bytes.size()), false)); }
std::uint64_t n_copyWithZone(id self, SEL, void *) { return n_copy(self, nullptr); }
std::uint64_t n_mutableCopy(id self, SEL) { return ret(data_create(dat(self)->bytes.data(), static_cast<CFIndex>(dat(self)->bytes.size()), true)); }
std::uint64_t n_mutableCopyWithZone(id self, SEL, void *) { return n_mutableCopy(self, nullptr); }
std::uint64_t n_description(id self, SEL) {
    std::string s = "<";
    static const char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < dat(self)->bytes.size(); i++) { if (i && i % 4 == 0) s += ' '; const unsigned char c = static_cast<unsigned char>(dat(self)->bytes[i]); s += hex[c >> 4]; s += hex[c & 15]; }
    s += ">";
    return ret(autorelease(string_from_utf8(s)));
}
std::uint64_t n_init(id self, SEL) { return ret(self); }
std::uint64_t n_initWithBytesLength(id self, SEL, const void *b, std::uint64_t n) { if (b) dat(self)->bytes.assign(static_cast<const char *>(b), n); else dat(self)->bytes.assign(n, '\0'); return ret(self); }
std::uint64_t n_initWithBytesNoCopy(id self, SEL, void *b, std::uint64_t n, std::uint64_t free_when_done) { dat(self)->bytes.assign(static_cast<const char *>(b), n); if (free_when_done) std::free(b); return ret(self); }
std::uint64_t n_initWithBytesNoCopy2(id self, SEL, void *b, std::uint64_t n) { return n_initWithBytesNoCopy(self, nullptr, b, n, 1); }
std::uint64_t n_initWithData(id self, SEL, id d) { if (d) dat(self)->bytes = dat(d)->bytes; return ret(self); }
std::uint64_t n_initWithCapacity(id self, SEL, std::uint64_t n) { dat(self)->bytes.reserve(n); return ret(self); }
std::uint64_t n_initWithLength(id self, SEL, std::uint64_t n) { dat(self)->bytes.assign(n, '\0'); return ret(self); }
std::uint64_t n_initWithContentsOfFile(id self, SEL, id path) {
    FILE *f = std::fopen(to_utf8(path).c_str(), "rb");
    if (!f) { release(self); return 0; }
    char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) dat(self)->bytes.append(buf, n);
    std::fclose(f);
    return ret(self);
}
std::uint64_t n_initWithContentsOfFileOptionsError(id self, SEL, id path, std::uint64_t, id *error) {
    const std::uint64_t r = n_initWithContentsOfFile(self, nullptr, path);
    if (!r && error) *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 260, nullptr));
    return r;
}
std::uint64_t n_initWithContentsOfURL(id self, SEL, id url) { id p = string_from_utf8(url_path(url)); const std::uint64_t r = n_initWithContentsOfFile(self, nullptr, p); release(p); return r; }
std::uint64_t n_writeToFileAtomically(id self, SEL, id path, std::uint64_t) {
    FILE *f = std::fopen(to_utf8(path).c_str(), "wb");
    if (!f) return 0;
    std::fwrite(dat(self)->bytes.data(), 1, dat(self)->bytes.size(), f);
    std::fclose(f);
    return 1;
}
std::uint64_t n_writeToFileOptionsError(id self, SEL, id path, std::uint64_t, id *error) {
    const std::uint64_t r = n_writeToFileAtomically(self, nullptr, path, 0);
    if (!r && error) *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 4, nullptr));
    return r;
}
std::uint64_t nc_data(id c, SEL) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "init")))); }
std::uint64_t nc_dataWithBytesLength(id c, SEL, const void *b, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithBytes:length:", ret(b), n)))); }
std::uint64_t nc_dataWithBytesNoCopy(id c, SEL, void *b, std::uint64_t n, std::uint64_t f) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithBytesNoCopy:length:freeWhenDone:", ret(b), n, f)))); }
std::uint64_t nc_dataWithData(id c, SEL, id d) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithData:", ret(d))))); }
std::uint64_t nc_dataWithContentsOfFile(id c, SEL, id p) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithContentsOfFile:", ret(p))))); }
std::uint64_t nc_dataWithContentsOfFileOptionsError(id c, SEL, id p, std::uint64_t o, id *e) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithContentsOfFile:options:error:", ret(p), o, ret(e))))); }
std::uint64_t nc_dataWithContentsOfURL(id c, SEL, id u) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithContentsOfURL:", ret(u))))); }
std::uint64_t nc_dataWithCapacity(id c, SEL, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithCapacity:", n)))); }
std::uint64_t nc_dataWithLength(id c, SEL, std::uint64_t n) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "initWithLength:", n)))); }
std::uint64_t mn_appendBytesLength(id self, SEL, const void *b, std::uint64_t n) { mutable_of<data_object>(self, "appendBytes:length:")->bytes.append(static_cast<const char *>(b), n); return 0; }
std::uint64_t mn_appendData(id self, SEL, id d) { mutable_of<data_object>(self, "appendData:")->bytes += dat(d)->bytes; return 0; }
std::uint64_t mn_setLength(id self, SEL, std::uint64_t n) { mutable_of<data_object>(self, "setLength:")->bytes.resize(n, '\0'); return 0; }
std::uint64_t mn_setData(id self, SEL, id d) { mutable_of<data_object>(self, "setData:")->bytes = dat(d)->bytes; return 0; }
std::uint64_t mn_replaceBytesInRange(id self, SEL, std::uint64_t loc, std::uint64_t len, const void *b) { auto &s = mutable_of<data_object>(self, "replaceBytesInRange:withBytes:")->bytes; if (loc <= s.size()) s.replace(loc, std::min<std::size_t>(len, s.size() - loc), static_cast<const char *>(b), len); return 0; }
std::uint64_t mn_increaseLengthBy(id self, SEL, std::uint64_t n) { auto &s = mutable_of<data_object>(self, "increaseLengthBy:")->bytes; s.resize(s.size() + n, '\0'); return 0; }

/* ---- NSNumber / NSValue ------------------------------------------------------ */

number_object *num(id n) { return as<number_object>(n); }
bool num_is_float(number_object *n) { return n->type == 'f' || n->type == 'd'; }
double num_double(number_object *n) { if (num_is_float(n)) return n->v.d; if (n->type == 'Q' || n->type == 'L' || n->type == 'I' || n->type == 'S' || n->type == 'C') return static_cast<double>(n->v.u); return static_cast<double>(n->v.i); }
std::int64_t num_int(number_object *n) { if (num_is_float(n)) return static_cast<std::int64_t>(n->v.d); return n->v.i; }
std::uint64_t nu_charValue(id self, SEL) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<signed char>(num_int(num(self))))); }
std::uint64_t nu_shortValue(id self, SEL) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<short>(num_int(num(self))))); }
std::uint64_t nu_intValue(id self, SEL) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<int>(num_int(num(self))))); }
std::uint64_t nu_longValue(id self, SEL) { return static_cast<std::uint64_t>(num_int(num(self))); }
std::uint64_t nu_unsignedValue(id self, SEL) { return static_cast<std::uint64_t>(num_int(num(self))); }
std::uint64_t nu_unsignedIntValue(id self, SEL) { return static_cast<std::uint32_t>(num_int(num(self))); }
std::uint64_t nu_boolValue(id self, SEL) { return num_is_float(num(self)) ? num(self)->v.d != 0 : num(self)->v.i != 0; }
double nu_doubleValue(id self, SEL) { return num_double(num(self)); }
float nu_floatValue(id self, SEL) { return static_cast<float>(num_double(num(self))); }
std::string number_string(number_object *n) {
    char buf[64];
    if (n->type == 'B') return n->v.i ? "1" : "0";
    if (n->type == 'd') { std::snprintf(buf, sizeof buf, "%.15g", n->v.d); return buf; }
    if (n->type == 'f') { std::snprintf(buf, sizeof buf, "%.7g", n->v.d); return buf; }
    if (n->type == 'Q' || n->type == 'L' || n->type == 'I') { std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(n->v.u)); return buf; }
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n->v.i));
    return buf;
}
std::uint64_t nu_stringValue(id self, SEL) { return ret(autorelease(string_from_utf8(number_string(num(self))))); }
std::uint64_t nu_description(id self, SEL) { return nu_stringValue(self, nullptr); }
std::uint64_t nu_objCType(id self, SEL) { static thread_local char t[2]; t[0] = num(self)->type; t[1] = 0; return ret(t); }
std::uint64_t nu_isEqualToNumber(id self, SEL, id o) {
    if (!o || !is_number(o)) return 0;
    if(constant_double_number(self)||constant_double_number(o))return number_double(self)==number_double(o);
    number_object *a = num(self), *b = num(o);
    if (num_is_float(a) || num_is_float(b)) return num_double(a) == num_double(b);
    return a->v.i == b->v.i;
}
std::uint64_t nu_isEqual(id self, SEL, id o) { return nu_isEqualToNumber(self, nullptr, o); }
std::uint64_t nu_hash(id self, SEL) { number_object *n = num(self); if (num_is_float(n) && n->v.d == std::floor(n->v.d) && std::fabs(n->v.d) < 9e18) return static_cast<std::uint64_t>(static_cast<std::int64_t>(n->v.d)); if (num_is_float(n)) { std::uint64_t b; std::memcpy(&b, &n->v.d, 8); return b; } return n->v.u; }
std::uint64_t nu_compare(id self, SEL, id o) {
    const double a = number_double(self), b = number_double(o);
    if(constant_double_number(self)||constant_double_number(o))return static_cast<std::uint64_t>(a<b?-1:a>b?1:0);
    if (!num_is_float(num(self)) && !num_is_float(num(o))) { const std::int64_t x = num(self)->v.i, y = num(o)->v.i; return static_cast<std::uint64_t>(x < y ? -1 : x > y ? 1 : 0); }
    return static_cast<std::uint64_t>(a < b ? -1 : a > b ? 1 : 0);
}
std::uint64_t nu_copyWithZone(id self, SEL, void *) { return ret(retain(self)); }
std::uint64_t nu_copy(id self, SEL) { return ret(retain(self)); }
std::uint64_t nu_getValue(id self, SEL, void *out) {
    number_object *n = num(self);
    switch (n->type) {
    case 'c': case 'C': case 'B': { const std::int8_t v = static_cast<std::int8_t>(n->v.i); std::memcpy(out, &v, 1); break; }
    case 's': case 'S': { const std::int16_t v = static_cast<std::int16_t>(n->v.i); std::memcpy(out, &v, 2); break; }
    case 'i': case 'I': { const std::int32_t v = static_cast<std::int32_t>(n->v.i); std::memcpy(out, &v, 4); break; }
    case 'f': { const float v = static_cast<float>(n->v.d); std::memcpy(out, &v, 4); break; }
    case 'd': std::memcpy(out, &n->v.d, 8); break;
    default: std::memcpy(out, &n->v.i, 8); break;
    }
    return 0;
}
id number_new(char type, std::int64_t i, double d) {
    auto *n = make<number_object>(g_cnumber);
    n->type = type;
    if (type == 'f' || type == 'd') n->v.d = d; else n->v.i = i;
    return reinterpret_cast<id>(n);
}
std::uint64_t nu_initWithChar(id self, SEL, std::uint64_t v) { num(self)->type = 'c'; num(self)->v.i = static_cast<signed char>(v); return ret(self); }
std::uint64_t nu_initWithUnsignedChar(id self, SEL, std::uint64_t v) { num(self)->type = 'C'; num(self)->v.i = static_cast<unsigned char>(v); return ret(self); }
std::uint64_t nu_initWithShort(id self, SEL, std::uint64_t v) { num(self)->type = 's'; num(self)->v.i = static_cast<short>(v); return ret(self); }
std::uint64_t nu_initWithUnsignedShort(id self, SEL, std::uint64_t v) { num(self)->type = 'S'; num(self)->v.i = static_cast<unsigned short>(v); return ret(self); }
std::uint64_t nu_initWithInt(id self, SEL, std::uint64_t v) { num(self)->type = 'i'; num(self)->v.i = static_cast<std::int32_t>(v); return ret(self); }
std::uint64_t nu_initWithUnsignedInt(id self, SEL, std::uint64_t v) { num(self)->type = 'I'; num(self)->v.i = static_cast<std::uint32_t>(v); return ret(self); }
std::uint64_t nu_initWithLong(id self, SEL, std::uint64_t v) { num(self)->type = 'q'; num(self)->v.i = static_cast<std::int64_t>(v); return ret(self); }
std::uint64_t nu_initWithUnsignedLong(id self, SEL, std::uint64_t v) { num(self)->type = 'Q'; num(self)->v.u = v; return ret(self); }
std::uint64_t nu_initWithBool(id self, SEL, std::uint64_t v) { num(self)->type = 'B'; num(self)->v.i = (v & 0xFF) != 0; return ret(self); }
std::uint64_t nu_initWithFloat(id self, SEL, float v) { num(self)->type = 'f'; num(self)->v.d = v; return ret(self); }
std::uint64_t nu_initWithDouble(id self, SEL, double v) { num(self)->type = 'd'; num(self)->v.d = v; return ret(self); }
/* Class factories: doubles arrive in v0 regardless of the integer args. */
std::uint64_t nc_numberWithChar(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('c', static_cast<signed char>(v), 0))); }
std::uint64_t nc_numberWithUnsignedChar(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('C', static_cast<unsigned char>(v), 0))); }
std::uint64_t nc_numberWithShort(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('s', static_cast<short>(v), 0))); }
std::uint64_t nc_numberWithUnsignedShort(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('S', static_cast<unsigned short>(v), 0))); }
std::uint64_t nc_numberWithInt(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('i', static_cast<std::int32_t>(v), 0))); }
std::uint64_t nc_numberWithUnsignedInt(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('I', static_cast<std::uint32_t>(v), 0))); }
std::uint64_t nc_numberWithLong(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('q', static_cast<std::int64_t>(v), 0))); }
std::uint64_t nc_numberWithUnsignedLong(id, SEL, std::uint64_t v) { return ret(autorelease(number_new('Q', static_cast<std::int64_t>(v), 0))); }
std::uint64_t nc_numberWithBool(id, SEL, std::uint64_t v) { return ret((v & 0xFF) ? kCFBooleanTrue_ptr : kCFBooleanFalse_ptr); }
std::uint64_t nc_numberWithFloat(id, SEL, float v) { return ret(autorelease(number_new('f', 0, v))); }
std::uint64_t nc_numberWithDouble(id, SEL, double v) { return ret(autorelease(number_new('d', 0, v))); }

/* NSValue: raw bytes with an objc type string. */
value_object *val(id v) { return as<value_object>(v); }
std::size_t type_size(std::string_view t) {
    if (t.empty()) return 0;
    switch (t[0]) {
    case 'c': case 'C': case 'B': return 1;
    case 's': case 'S': return 2;
    case 'i': case 'I': case 'f': return 4;
    case 'q': case 'Q': case 'l': case 'L': case 'd': case '@': case '#': case ':': case '^': case '*': return 8;
    case '{': {
        /* {name=fields}: sum of field sizes (adequate for all-scalar structs like NSRect). */
        std::size_t total = 0;
        const auto eq = t.find('=');
        if (eq == std::string_view::npos) return 0;
        for (std::size_t i = eq + 1; i < t.size() && t[i] != '}';) {
            if (t[i] == '{') { std::size_t depth = 0, j = i; do { if (t[j] == '{') depth++; else if (t[j] == '}') depth--; j++; } while (depth && j < t.size()); total += type_size(t.substr(i, j - i)); i = j; }
            else { total += type_size(t.substr(i, 1)); i++; }
        }
        return total;
    }
    default: return 8;
    }
}
std::uint64_t v_initWithBytesObjCType(id self, SEL, const void *bytes, const char *type) { val(self)->type = type ? type : ""; val(self)->bytes.assign(static_cast<const char *>(bytes), type_size(val(self)->type)); return ret(self); }
std::uint64_t v_getValue(id self, SEL, void *out) { std::memcpy(out, val(self)->bytes.data(), val(self)->bytes.size()); return 0; }
std::uint64_t v_getValueSize(id self, SEL, void *out, std::uint64_t n) { std::memcpy(out, val(self)->bytes.data(), std::min<std::size_t>(n, val(self)->bytes.size())); return 0; }
std::uint64_t v_objCType(id self, SEL) { return ret(val(self)->type.c_str()); }
std::uint64_t v_pointerValue(id self, SEL) { std::uint64_t p = 0; std::memcpy(&p, val(self)->bytes.data(), std::min<std::size_t>(8, val(self)->bytes.size())); return p; }
std::uint64_t v_isEqualToValue(id self, SEL, id o) { return o && is_kind(o, g_value) && !is_number(o) && val(self)->bytes == val(o)->bytes && val(self)->type == val(o)->type; }
std::uint64_t v_hash(id self, SEL) { std::uint64_t h = 1469598103934665603ull; for (unsigned char c : val(self)->bytes) { h ^= c; h *= 1099511628211ull; } return h; }
std::uint64_t v_copyWithZone(id self, SEL, void *) { return ret(retain(self)); }
mm_two_doubles v_pointValue(id self, SEL) {
    double p[2] = {0, 0};
    std::memcpy(p, val(self)->bytes.data(), std::min<std::size_t>(16, val(self)->bytes.size()));
    return {p[0], p[1]};
}
mm_two_doubles v_rectValue(id self, SEL) { double p[4] = {0, 0, 0, 0}; std::memcpy(p, val(self)->bytes.data(), std::min<std::size_t>(32, val(self)->bytes.size())); mm_objc_return_doubles_hi(p[2], p[3]); return {p[0], p[1]}; }
std::uint64_t v_rangeValue(id self, SEL) { std::uint64_t p[2] = {0, 0}; std::memcpy(p, val(self)->bytes.data(), std::min<std::size_t>(16, val(self)->bytes.size()));
    mm_objc_return_words_hi(p[1]);
    return p[0]; }
id value_new(const void *bytes, std::size_t n, const char *type) { auto *v = make<value_object>(g_value); v->type = type; v->bytes.assign(static_cast<const char *>(bytes), n); return autorelease(reinterpret_cast<id>(v)); }
std::uint64_t vc_valueWithBytesObjCType(id, SEL, const void *bytes, const char *type) { return ret(value_new(bytes, type_size(type), type)); }
std::uint64_t vc_valueWithPointer(id, SEL, std::uint64_t p) { return ret(value_new(&p, 8, "^v")); }
std::uint64_t vc_valueWithNonretainedObject(id, SEL, std::uint64_t p) { return ret(value_new(&p, 8, "@")); }
std::uint64_t vc_valueWithPoint(id, SEL, double x, double y) { const double p[2] = {x, y}; return ret(value_new(p, 16, "{CGPoint=dd}")); }
std::uint64_t vc_valueWithSize(id, SEL, double x, double y) { const double p[2] = {x, y}; return ret(value_new(p, 16, "{CGSize=dd}")); }
std::uint64_t vc_valueWithRect(id, SEL, double x, double y, double w, double h) { const double p[4] = {x, y, w, h}; return ret(value_new(p, 32, "{CGRect={CGPoint=dd}{CGSize=dd}}")); }
std::uint64_t vc_valueWithRange(id, SEL, std::uint64_t loc, std::uint64_t len) { const std::uint64_t p[2] = {loc, len}; return ret(value_new(p, 16, "{_NSRange=QQ}")); }

/* NSNull */
std::uint64_t null_null(id, SEL) { return ret(kCFNull_ptr); }
std::uint64_t null_description(id, SEL) { return ret(constant_string("<null>")); }

} // namespace

/* ---- shared helpers ------------------------------------------------------- */

bool objects_equal(id a, id b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return send(a, "isEqual:", ret(b)) != 0;
}
std::uint64_t object_hash(id o) { return o ? send(o, "hash") : 0; }

id array_create(const id *objects, CFIndex n, bool mutable_) {
    auto *a = make<array_object>(g_carray);
    a->mutable_ = mutable_;
    for (CFIndex i = 0; i < n; i++) a->items.push_back(retain(objects[i]));
    return reinterpret_cast<id>(a);
}
std::vector<id> &array_storage(id a) { return arr(a)->items; }
bool is_array(id o) { return o && is_kind(o, g_array); }

id dictionary_create(bool mutable_) { auto *d = make<dict_object>(g_cdict); d->mutable_ = mutable_; return reinterpret_cast<id>(d); }
void dictionary_set(id d, id key, id value) {
    dict_object *o = dict(d);
    auto it = o->map.find(key);
    if (it != o->map.end()) { id old = it->second; it->second = retain(value); release(old); return; }
    id k = copy_key(key);
    o->map.emplace(k, retain(value));
    o->order.push_back(k);
}
id dictionary_get(id d, id key) { auto &m = dict(d)->map; auto it = m.find(key); return it == m.end() ? nullptr : it->second; }
void dictionary_remove(id d, id key) {
    dict_object *o = dict(d);
    auto it = o->map.find(key);
    if (it == o->map.end()) return;
    id k = it->first, v = it->second;
    o->map.erase(it);
    o->order.erase(std::find(o->order.begin(), o->order.end(), k));
    release(v); release(k);
}
CFIndex dictionary_count(id d) { return static_cast<CFIndex>(dict(d)->map.size()); }
void dictionary_keys(id d, std::vector<id> &keys) { keys = dict(d)->order; }
bool is_dictionary(id o) { return o && is_kind(o, g_dict); }

id data_create(const void *bytes, CFIndex n, bool mutable_) {
    auto *d = make<data_object>(g_cdata);
    d->mutable_ = mutable_;
    if (bytes) d->bytes.assign(static_cast<const char *>(bytes), static_cast<std::size_t>(n)); else d->bytes.assign(static_cast<std::size_t>(n), '\0');
    return reinterpret_cast<id>(d);
}
std::string &data_storage(id d) { return dat(d)->bytes; }
bool is_data(id o) { return o && is_kind(o, g_data); }

id number_from_int64(std::int64_t v) { return number_new('q', v, 0); }
id number_from_double(double v) { return number_new('d', 0, v); }
id number_bool(bool v) { return v ? kCFBooleanTrue_ptr : kCFBooleanFalse_ptr; }
bool is_number(id o) { return o && is_kind(o, g_number); }
double number_double(id n) { return constant_double_number(n)?as<constant_double>(n)->value:num_double(num(n)); }
std::int64_t number_int64(id n) { return constant_double_number(n)?static_cast<std::int64_t>(as<constant_double>(n)->value):num_int(num(n)); }
bool number_is_float(id n) { return constant_double_number(n)||num_is_float(num(n)); }
id null_object() { return kCFNull_ptr; }

namespace {

struct array_dtor : array_object { ~array_dtor() { for (id o : items) release(o); } };
struct dict_dtor : dict_object { ~dict_dtor() { for (auto &[k, v] : map) { release(v); release(k); } } };
struct set_dtor : set_object { ~set_dtor() { for (id o : items) release(o); } };

} // namespace

void register_collections() {
    Class nsobject = cls("NSObject");
    g_array = mm_objc_define_class("NSArray", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSArray), FND_META_REF(NSArray));
    mm_objc_define_class("NSConstantArray", g_array, sizeof(constant_array), FND_CLASS_REF(NSConstantArray), FND_META_REF(NSConstantArray));
    g_marray = mm_objc_define_class("NSMutableArray", g_array, sizeof(mm_objc_object), FND_CLASS_REF(NSMutableArray), FND_META_REF(NSMutableArray));
    g_carray = mm_objc_define_class("__NSArrayM", g_marray, sizeof(array_object), FND_CLASS_REF(__NSArrayM), FND_META_REF(__NSArrayM));
    register_lifecycle<array_dtor>(g_carray);
    register_cluster<array_dtor>(g_array, g_carray, false);
    register_cluster<array_dtor>(g_marray, g_carray, true);
    g_enumerator = mm_objc_define_class("NSEnumerator", nsobject, sizeof(enumerator_object), FND_CLASS_REF(NSEnumerator), FND_META_REF(NSEnumerator));
    register_lifecycle<enumerator_object_dtor>(g_enumerator);
    FND_METHODS(g_enumerator,
        {"nextObject", IMP(e_nextObject), "@16@0:8"},
        {"allObjects", IMP(e_allObjects), "@16@0:8"},
        {"countByEnumeratingWithState:objects:count:", IMP(e_countByEnumerating), "Q40@0:8^{?=Q^@^Q[5Q]}16^@24Q32"});

    FND_METHODS(g_array,
        {"count", IMP(a_count), "Q16@0:8"},
        {"objectAtIndex:", IMP(a_objectAtIndex), "@24@0:8Q16"},
        {"objectAtIndexedSubscript:", IMP(a_objectAtIndex), "@24@0:8Q16"},
        {"firstObject", IMP(a_firstObject), "@16@0:8"},
        {"lastObject", IMP(a_lastObject), "@16@0:8"},
        {"containsObject:", IMP(a_containsObject), "B24@0:8@16"},
        {"indexOfObject:", IMP(a_indexOfObject), "Q24@0:8@16"},
        {"indexOfObjectIdenticalTo:", IMP(a_indexOfObjectIdenticalTo), "Q24@0:8@16"},
        {"getObjects:range:", IMP(a_getObjectsRange), "v40@0:8^@16{_NSRange=QQ}24"},
        {"isEqualToArray:", IMP(a_isEqualToArray), "B24@0:8@16"},
        {"isEqual:", IMP(a_isEqualToArray), "B24@0:8@16"},
        {"hash", IMP(a_hash), "Q16@0:8"},
        {"copy", IMP(a_copy), "@16@0:8"},
        {"copyWithZone:", IMP(a_copyWithZone), "@24@0:8^v16"},
        {"mutableCopy", IMP(a_mutableCopy), "@16@0:8"},
        {"mutableCopyWithZone:", IMP(a_mutableCopyWithZone), "@24@0:8^v16"},
        {"subarrayWithRange:", IMP(a_subarrayWithRange), "@32@0:8{_NSRange=QQ}16"},
        {"arrayByAddingObject:", IMP(a_arrayByAddingObject), "@24@0:8@16"},
        {"arrayByAddingObjectsFromArray:", IMP(a_arrayByAddingObjectsFromArray), "@24@0:8@16"},
        {"componentsJoinedByString:", IMP(a_componentsJoinedByString), "@24@0:8@16"},
        {"description", IMP(a_description), "@16@0:8"},
        {"countByEnumeratingWithState:objects:count:", IMP(a_countByEnumerating), "Q40@0:8^{?=Q^@^Q[5Q]}16^@24Q32"},
        {"enumerateObjectsUsingBlock:", IMP(a_enumerateObjectsUsingBlock), "v24@0:8@?16"},
        {"sortedArrayUsingComparator:", IMP(a_sortedArrayUsingComparator), "@24@0:8@?16"},
        {"sortedArrayUsingSelector:", IMP(a_sortedArrayUsingSelector), "@24@0:8:16"},
        {"objectEnumerator", IMP(a_objectEnumerator), "@16@0:8"},
        {"reverseObjectEnumerator", IMP(a_reverseObjectEnumerator), "@16@0:8"},
        {"init", IMP(a_init), "@16@0:8"},
        {"initWithObjects:count:", IMP(a_initWithObjectsCount), "@32@0:8^@16Q24"},
        {"initWithObjects:", IMP(a_initWithObjects), "@24@0:8@16"},
        {"initWithArray:", IMP(a_initWithArray), "@24@0:8@16"},
        {"initWithArray:copyItems:", IMP(a_initWithArrayCopyItems), "@32@0:8@16B24"},
        {"initWithCapacity:", IMP(a_initWithCapacity), "@24@0:8Q16"},
        {"initWithContentsOfFile:", IMP(a_initWithContentsOfFile), "@24@0:8@16"},
        {"writeToFile:atomically:", IMP(a_writeToFileAtomically), "B32@0:8@16B24"});
    FND_CLASS_METHODS(g_array,
        {"array", IMP(ac_array), "@16@0:8"},
        {"arrayWithObject:", IMP(ac_arrayWithObject), "@24@0:8@16"},
        {"arrayWithObjects:count:", IMP(ac_arrayWithObjectsCount), "@32@0:8^@16Q24"},
        {"arrayWithObjects:", IMP(ac_arrayWithObjects), "@24@0:8@16"},
        {"arrayWithArray:", IMP(ac_arrayWithArray), "@24@0:8@16"},
        {"arrayWithCapacity:", IMP(ac_arrayWithCapacity), "@24@0:8Q16"},
        {"arrayWithContentsOfFile:", IMP(ac_arrayWithContentsOfFile), "@24@0:8@16"});
    FND_METHODS(g_marray,
        {"addObject:", IMP(ma_addObject), "v24@0:8@16"},
        {"addObjectsFromArray:", IMP(ma_addObjectsFromArray), "v24@0:8@16"},
        {"insertObject:atIndex:", IMP(ma_insertObjectAtIndex), "v32@0:8@16Q24"},
        {"removeObjectAtIndex:", IMP(ma_removeObjectAtIndex), "v24@0:8Q16"},
        {"removeLastObject", IMP(ma_removeLastObject), "v16@0:8"},
        {"removeObject:", IMP(ma_removeObject), "v24@0:8@16"},
        {"removeObjectIdenticalTo:", IMP(ma_removeObjectIdenticalTo), "v24@0:8@16"},
        {"removeAllObjects", IMP(ma_removeAllObjects), "v16@0:8"},
        {"removeObjectsInRange:", IMP(ma_removeObjectsInRange), "v32@0:8{_NSRange=QQ}16"},
        {"replaceObjectAtIndex:withObject:", IMP(ma_replaceObjectAtIndex), "v32@0:8Q16@24"},
        {"setObject:atIndexedSubscript:", IMP(ma_replaceObjectAtIndex), "v32@0:8@16Q24"},
        {"setArray:", IMP(ma_setArray), "v24@0:8@16"},
        {"exchangeObjectAtIndex:withObjectAtIndex:", IMP(ma_exchange), "v32@0:8Q16Q24"},
        {"sortUsingComparator:", IMP(ma_sortUsingComparator), "v24@0:8@?16"},
        {"sortUsingSelector:", IMP(ma_sortUsingSelector), "v24@0:8:16"});
    /* setObject:atIndexedSubscript: has (object, index) order; wrap. */
    static auto set_subscript = [](id self, SEL, id o, std::uint64_t i) -> std::uint64_t {
        auto &v = mutable_of<array_object>(self, "setObject:atIndexedSubscript:")->items;
        if (i == v.size()) return ma_addObject(self, nullptr, o);
        return ma_replaceObjectAtIndex(self, nullptr, i, o);
    };
    mm_objc_add_method(g_marray, "setObject:atIndexedSubscript:", IMP(+set_subscript), "v32@0:8@16Q24");

    g_dict = mm_objc_define_class("NSDictionary", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSDictionary), FND_META_REF(NSDictionary));
    g_mdict = mm_objc_define_class("NSMutableDictionary", g_dict, sizeof(mm_objc_object), FND_CLASS_REF(NSMutableDictionary), FND_META_REF(NSMutableDictionary));
    g_cdict = mm_objc_define_class("__NSDictionaryM", g_mdict, sizeof(dict_object), FND_CLASS_REF(__NSDictionaryM), FND_META_REF(__NSDictionaryM));
    register_lifecycle<dict_dtor>(g_cdict);
    register_cluster<dict_dtor>(g_dict, g_cdict, false);
    register_cluster<dict_dtor>(g_mdict, g_cdict, true);
    FND_METHODS(g_dict,
        {"count", IMP(d_count), "Q16@0:8"},
        {"objectForKey:", IMP(d_objectForKey), "@24@0:8@16"},
        {"objectForKeyedSubscript:", IMP(d_objectForKeyedSubscript), "@24@0:8@16"},
        {"valueForKey:", IMP(d_valueForKey), "@24@0:8@16"},
        {"allKeys", IMP(d_allKeys), "@16@0:8"},
        {"allValues", IMP(d_allValues), "@16@0:8"},
        {"keyEnumerator", IMP(d_keyEnumerator), "@16@0:8"},
        {"objectEnumerator", IMP(d_objectEnumerator), "@16@0:8"},
        {"countByEnumeratingWithState:objects:count:", IMP(d_countByEnumerating), "Q40@0:8^{?=Q^@^Q[5Q]}16^@24Q32"},
        {"enumerateKeysAndObjectsUsingBlock:", IMP(d_enumerateKeysAndObjects), "v24@0:8@?16"},
        {"isEqualToDictionary:", IMP(d_isEqualToDictionary), "B24@0:8@16"},
        {"isEqual:", IMP(d_isEqualToDictionary), "B24@0:8@16"},
        {"hash", IMP(d_hash), "Q16@0:8"},
        {"copy", IMP(d_copy), "@16@0:8"},
        {"copyWithZone:", IMP(d_copyWithZone), "@24@0:8^v16"},
        {"mutableCopy", IMP(d_mutableCopy), "@16@0:8"},
        {"mutableCopyWithZone:", IMP(d_mutableCopyWithZone), "@24@0:8^v16"},
        {"description", IMP(d_description), "@16@0:8"},
        {"init", IMP(d_init), "@16@0:8"},
        {"initWithObjects:forKeys:count:", IMP(d_initWithObjectsForKeysCount), "@40@0:8^@16^@24Q32"},
        {"initWithObjects:forKeys:", IMP(d_initWithObjectsForKeys), "@32@0:8@16@24"},
        {"initWithDictionary:", IMP(d_initWithDictionary), "@24@0:8@16"},
        {"initWithDictionary:copyItems:", IMP(d_initWithDictionaryCopyItems), "@32@0:8@16B24"},
        {"initWithObjectsAndKeys:", IMP(d_initWithObjectsAndKeys), "@24@0:8@16"},
        {"initWithCapacity:", IMP(d_initWithCapacity), "@24@0:8Q16"},
        {"initWithContentsOfFile:", IMP(d_initWithContentsOfFile), "@24@0:8@16"},
        {"writeToFile:atomically:", IMP(d_writeToFileAtomically), "B32@0:8@16B24"});
    FND_CLASS_METHODS(g_dict,
        {"dictionary", IMP(dc_dictionary), "@16@0:8"},
        {"dictionaryWithObject:forKey:", IMP(dc_dictionaryWithObjectForKey), "@32@0:8@16@24"},
        {"dictionaryWithObjects:forKeys:count:", IMP(dc_dictionaryWithObjectsForKeysCount), "@40@0:8^@16^@24Q32"},
        {"dictionaryWithObjects:forKeys:", IMP(dc_dictionaryWithObjectsForKeys), "@32@0:8@16@24"},
        {"dictionaryWithDictionary:", IMP(dc_dictionaryWithDictionary), "@24@0:8@16"},
        {"dictionaryWithObjectsAndKeys:", IMP(dc_dictionaryWithObjectsAndKeys), "@24@0:8@16"},
        {"dictionaryWithCapacity:", IMP(dc_dictionaryWithCapacity), "@24@0:8Q16"},
        {"dictionaryWithContentsOfFile:", IMP(dc_dictionaryWithContentsOfFile), "@24@0:8@16"});
    FND_METHODS(g_mdict,
        {"setObject:forKey:", IMP(md_setObjectForKey), "v32@0:8@16@24"},
        {"setObject:forKeyedSubscript:", IMP(md_setObjectForKeyedSubscript), "v32@0:8@16@24"},
        {"setValue:forKey:", IMP(md_setValueForKey), "v32@0:8@16@24"},
        {"removeObjectForKey:", IMP(md_removeObjectForKey), "v24@0:8@16"},
        {"removeAllObjects", IMP(md_removeAllObjects), "v16@0:8"},
        {"addEntriesFromDictionary:", IMP(md_addEntriesFromDictionary), "v24@0:8@16"},
        {"setDictionary:", IMP(md_setDictionary), "v24@0:8@16"});

    g_set = mm_objc_define_class("NSSet", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSSet), FND_META_REF(NSSet));
    g_mset = mm_objc_define_class("NSMutableSet", g_set, sizeof(mm_objc_object), FND_CLASS_REF(NSMutableSet), FND_META_REF(NSMutableSet));
    g_cset = mm_objc_define_class("__NSSetM", g_mset, sizeof(set_object), FND_CLASS_REF(__NSSetM), FND_META_REF(__NSSetM));
    register_lifecycle<set_dtor>(g_cset);
    register_cluster<set_dtor>(g_set, g_cset, false);
    register_cluster<set_dtor>(g_mset, g_cset, true);
    FND_METHODS(g_set,
        {"count", IMP(s_count), "Q16@0:8"},
        {"containsObject:", IMP(s_containsObject), "B24@0:8@16"},
        {"member:", IMP(s_member), "@24@0:8@16"},
        {"allObjects", IMP(s_allObjects), "@16@0:8"},
        {"anyObject", IMP(s_anyObject), "@16@0:8"},
        {"objectEnumerator", IMP(s_objectEnumerator), "@16@0:8"},
        {"countByEnumeratingWithState:objects:count:", IMP(s_countByEnumerating), "Q40@0:8^{?=Q^@^Q[5Q]}16^@24Q32"},
        {"init", IMP(s_init), "@16@0:8"},
        {"initWithObjects:count:", IMP(s_initWithObjectsCount), "@32@0:8^@16Q24"},
        {"initWithObjects:", IMP(s_initWithObjects), "@24@0:8@16"},
        {"initWithArray:", IMP(s_initWithArray), "@24@0:8@16"},
        {"initWithSet:", IMP(s_initWithSet), "@24@0:8@16"},
        {"copy", IMP(s_copy), "@16@0:8"},
        {"copyWithZone:", IMP(s_copyWithZone), "@24@0:8^v16"},
        {"mutableCopy", IMP(s_mutableCopy), "@16@0:8"},
        {"mutableCopyWithZone:", IMP(s_mutableCopyWithZone), "@24@0:8^v16"},
        {"isEqualToSet:", IMP(s_isEqualToSet), "B24@0:8@16"},
        {"isEqual:", IMP(s_isEqualToSet), "B24@0:8@16"},
        {"hash", IMP(s_count), "Q16@0:8"});
    FND_CLASS_METHODS(g_set,
        {"set", IMP(sc_set), "@16@0:8"},
        {"setWithObject:", IMP(sc_setWithObject), "@24@0:8@16"},
        {"setWithArray:", IMP(sc_setWithArray), "@24@0:8@16"},
        {"setWithObjects:", IMP(sc_setWithObjects), "@24@0:8@16"});
    FND_METHODS(g_mset,
        {"initWithCapacity:", IMP(s_initWithCapacity), "@24@0:8Q16"},
        {"addObject:", IMP(ms_addObject), "v24@0:8@16"},
        {"removeObject:", IMP(ms_removeObject), "v24@0:8@16"},
        {"removeAllObjects", IMP(ms_removeAllObjects), "v16@0:8"},
        {"addObjectsFromArray:", IMP(ms_addObjectsFromArray), "v24@0:8@16"},
        {"unionSet:", IMP(ms_unionSet), "v24@0:8@16"},
        {"minusSet:", IMP(ms_minusSet), "v24@0:8@16"});
    FND_CLASS_METHODS(g_mset, {"setWithCapacity:", IMP(sc_setWithCapacity), "@24@0:8Q16"});

    g_data = mm_objc_define_class("NSData", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSData), FND_META_REF(NSData));
    g_mdata = mm_objc_define_class("NSMutableData", g_data, sizeof(mm_objc_object), FND_CLASS_REF(NSMutableData), FND_META_REF(NSMutableData));
    g_cdata = mm_objc_define_class("__NSDataM", g_mdata, sizeof(data_object), FND_CLASS_REF(__NSDataM), FND_META_REF(__NSDataM));
    register_lifecycle<data_object>(g_cdata);
    register_cluster<data_object>(g_data, g_cdata, false);
    register_cluster<data_object>(g_mdata, g_cdata, true);
    FND_METHODS(g_data,
        {"length", IMP(n_length), "Q16@0:8"},
        {"bytes", IMP(n_bytes), "r^v16@0:8"},
        {"getBytes:length:", IMP(n_getBytesLength), "v32@0:8^v16Q24"},
        {"getBytes:range:", IMP(n_getBytesRange), "v40@0:8^v16{_NSRange=QQ}24"},
        {"isEqualToData:", IMP(n_isEqualToData), "B24@0:8@16"},
        {"isEqual:", IMP(n_isEqualToData), "B24@0:8@16"},
        {"hash", IMP(n_hash), "Q16@0:8"},
        {"subdataWithRange:", IMP(n_subdataWithRange), "@32@0:8{_NSRange=QQ}16"},
        {"copy", IMP(n_copy), "@16@0:8"},
        {"copyWithZone:", IMP(n_copyWithZone), "@24@0:8^v16"},
        {"mutableCopy", IMP(n_mutableCopy), "@16@0:8"},
        {"mutableCopyWithZone:", IMP(n_mutableCopyWithZone), "@24@0:8^v16"},
        {"description", IMP(n_description), "@16@0:8"},
        {"init", IMP(n_init), "@16@0:8"},
        {"initWithBytes:length:", IMP(n_initWithBytesLength), "@32@0:8r^v16Q24"},
        {"initWithBytesNoCopy:length:freeWhenDone:", IMP(n_initWithBytesNoCopy), "@40@0:8^v16Q24B32"},
        {"initWithBytesNoCopy:length:", IMP(n_initWithBytesNoCopy2), "@32@0:8^v16Q24"},
        {"initWithData:", IMP(n_initWithData), "@24@0:8@16"},
        {"initWithCapacity:", IMP(n_initWithCapacity), "@24@0:8Q16"},
        {"initWithLength:", IMP(n_initWithLength), "@24@0:8Q16"},
        {"initWithContentsOfFile:", IMP(n_initWithContentsOfFile), "@24@0:8@16"},
        {"initWithContentsOfFile:options:error:", IMP(n_initWithContentsOfFileOptionsError), "@40@0:8@16Q24^@32"},
        {"initWithContentsOfURL:", IMP(n_initWithContentsOfURL), "@24@0:8@16"},
        {"writeToFile:atomically:", IMP(n_writeToFileAtomically), "B32@0:8@16B24"},
        {"writeToFile:options:error:", IMP(n_writeToFileOptionsError), "B40@0:8@16Q24^@32"});
    FND_CLASS_METHODS(g_data,
        {"data", IMP(nc_data), "@16@0:8"},
        {"dataWithBytes:length:", IMP(nc_dataWithBytesLength), "@32@0:8r^v16Q24"},
        {"dataWithBytesNoCopy:length:freeWhenDone:", IMP(nc_dataWithBytesNoCopy), "@40@0:8^v16Q24B32"},
        {"dataWithData:", IMP(nc_dataWithData), "@24@0:8@16"},
        {"dataWithContentsOfFile:", IMP(nc_dataWithContentsOfFile), "@24@0:8@16"},
        {"dataWithContentsOfFile:options:error:", IMP(nc_dataWithContentsOfFileOptionsError), "@40@0:8@16Q24^@32"},
        {"dataWithContentsOfURL:", IMP(nc_dataWithContentsOfURL), "@24@0:8@16"},
        {"dataWithCapacity:", IMP(nc_dataWithCapacity), "@24@0:8Q16"},
        {"dataWithLength:", IMP(nc_dataWithLength), "@24@0:8Q16"});
    FND_METHODS(g_mdata,
        {"mutableBytes", IMP(n_mutableBytes), "^v16@0:8"},
        {"appendBytes:length:", IMP(mn_appendBytesLength), "v32@0:8r^v16Q24"},
        {"appendData:", IMP(mn_appendData), "v24@0:8@16"},
        {"setLength:", IMP(mn_setLength), "v24@0:8Q16"},
        {"setData:", IMP(mn_setData), "v24@0:8@16"},
        {"replaceBytesInRange:withBytes:", IMP(mn_replaceBytesInRange), "v40@0:8{_NSRange=QQ}16r^v32"},
        {"increaseLengthBy:", IMP(mn_increaseLengthBy), "v24@0:8Q16"});

    g_value = mm_objc_define_class("NSValue", nsobject, sizeof(value_object), FND_CLASS_REF(NSValue), FND_META_REF(NSValue));
    register_lifecycle<value_object>(g_value);
    FND_METHODS(g_value,
        {"initWithBytes:objCType:", IMP(v_initWithBytesObjCType), "@32@0:8r^v16r*24"},
        {"getValue:", IMP(v_getValue), "v24@0:8^v16"},
        {"getValue:size:", IMP(v_getValueSize), "v32@0:8^v16Q24"},
        {"objCType", IMP(v_objCType), "r*16@0:8"},
        {"pointerValue", IMP(v_pointerValue), "^v16@0:8"},
        {"nonretainedObjectValue", IMP(v_pointerValue), "@16@0:8"},
        {"isEqualToValue:", IMP(v_isEqualToValue), "B24@0:8@16"},
        {"isEqual:", IMP(v_isEqualToValue), "B24@0:8@16"},
        {"hash", IMP(v_hash), "Q16@0:8"},
        {"copyWithZone:", IMP(v_copyWithZone), "@24@0:8^v16"},
        {"pointValue", IMP(v_pointValue), "{CGPoint=dd}16@0:8"},
        {"sizeValue", IMP(v_pointValue), "{CGSize=dd}16@0:8"},
        {"rectValue", IMP(v_rectValue), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"rangeValue", IMP(v_rangeValue), "{_NSRange=QQ}16@0:8"});
    FND_CLASS_METHODS(g_value,
        {"valueWithBytes:objCType:", IMP(vc_valueWithBytesObjCType), "@32@0:8r^v16r*24"},
        {"value:withObjCType:", IMP(vc_valueWithBytesObjCType), "@32@0:8r^v16r*24"},
        {"valueWithPointer:", IMP(vc_valueWithPointer), "@24@0:8r^v16"},
        {"valueWithNonretainedObject:", IMP(vc_valueWithNonretainedObject), "@24@0:8@16"},
        {"valueWithPoint:", IMP(vc_valueWithPoint), "@32@0:8{CGPoint=dd}16"},
        {"valueWithSize:", IMP(vc_valueWithSize), "@32@0:8{CGSize=dd}16"},
        {"valueWithRect:", IMP(vc_valueWithRect), "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"valueWithRange:", IMP(vc_valueWithRange), "@32@0:8{_NSRange=QQ}16"});

    g_number = mm_objc_define_class("NSNumber", g_value, sizeof(number_object), FND_CLASS_REF(NSNumber), FND_META_REF(NSNumber));
    g_cnumber = mm_objc_define_class("__NSCFNumber", g_number, sizeof(number_object), FND_CLASS_REF(__NSCFNumber), FND_META_REF(__NSCFNumber));
    g_boolean = mm_objc_define_class("__NSCFBoolean", g_cnumber, sizeof(number_object), FND_CLASS_REF(__NSCFBoolean), FND_META_REF(__NSCFBoolean));
    mm_objc_define_class("NSConstantIntegerNumber", g_cnumber, sizeof(number_object), FND_CLASS_REF(NSConstantIntegerNumber), FND_META_REF(NSConstantIntegerNumber));
    g_constant_double=mm_objc_define_class("NSConstantDoubleNumber",g_number,sizeof(constant_double),FND_CLASS_REF(NSConstantDoubleNumber),FND_META_REF(NSConstantDoubleNumber));
    static auto double_value=[](id s,SEL){return as<constant_double>(s)->value;};
    static auto float_value=[](id s,SEL){return static_cast<float>(as<constant_double>(s)->value);};
    static auto int_value=[](id s,SEL){return static_cast<std::int64_t>(as<constant_double>(s)->value);};
    static auto bool_value=[](id s,SEL){return std::uint64_t(as<constant_double>(s)->value!=0);};
    static auto type=[](id,SEL){return ret("d");};
    static auto text=[](id s,SEL){char b[96];std::snprintf(b,sizeof b,"%.17g",as<constant_double>(s)->value);return ret(autorelease(string_from_utf8(b)));};
    static auto hash=[](id s,SEL){const double v=as<constant_double>(s)->value;if(std::isfinite(v)&&std::fabs(v)<9e18&&v==std::floor(v))return static_cast<std::uint64_t>(static_cast<std::int64_t>(v));std::uint64_t bits;std::memcpy(&bits,&v,8);return bits;};
    static auto compare=[](id s,SEL,id o){const double a=number_double(s),b=number_double(o);return std::int64_t(a<b?-1:a>b?1:0);};
    static auto value=[](id s,SEL,void *out)->std::uint64_t{if(out)std::memcpy(out,&as<constant_double>(s)->value,8);return 0;};
    FND_METHODS(g_constant_double,
        {"doubleValue",IMP(+double_value),"d16@0:8"},{"floatValue",IMP(+float_value),"f16@0:8"},
        {"integerValue",IMP(+int_value),"q16@0:8"},{"longLongValue",IMP(+int_value),"q16@0:8"},{"intValue",IMP(+int_value),"i16@0:8"},{"boolValue",IMP(+bool_value),"B16@0:8"},
        {"objCType",IMP(+type),"r*16@0:8"},{"description",IMP(+text),"@16@0:8"},{"stringValue",IMP(+text),"@16@0:8"},
        {"hash",IMP(+hash),"Q16@0:8"},{"compare:",IMP(+compare),"q24@0:8@16"},{"getValue:",IMP(+value),"v24@0:8^v16"});
    register_lifecycle<number_object>(g_cnumber);
    register_cluster<number_object>(g_number, g_cnumber, false);
    FND_METHODS(g_number,
        {"charValue", IMP(nu_charValue), "c16@0:8"},
        {"unsignedCharValue", IMP(nu_charValue), "C16@0:8"},
        {"shortValue", IMP(nu_shortValue), "s16@0:8"},
        {"unsignedShortValue", IMP(nu_shortValue), "S16@0:8"},
        {"intValue", IMP(nu_intValue), "i16@0:8"},
        {"unsignedIntValue", IMP(nu_unsignedIntValue), "I16@0:8"},
        {"longValue", IMP(nu_longValue), "q16@0:8"},
        {"longLongValue", IMP(nu_longValue), "q16@0:8"},
        {"integerValue", IMP(nu_longValue), "q16@0:8"},
        {"unsignedLongValue", IMP(nu_unsignedValue), "Q16@0:8"},
        {"unsignedLongLongValue", IMP(nu_unsignedValue), "Q16@0:8"},
        {"unsignedIntegerValue", IMP(nu_unsignedValue), "Q16@0:8"},
        {"boolValue", IMP(nu_boolValue), "B16@0:8"},
        {"doubleValue", IMP(nu_doubleValue), "d16@0:8"},
        {"floatValue", IMP(nu_floatValue), "f16@0:8"},
        {"stringValue", IMP(nu_stringValue), "@16@0:8"},
        {"description", IMP(nu_description), "@16@0:8"},
        {"descriptionWithLocale:", IMP(nu_description), "@24@0:8@16"},
        {"objCType", IMP(nu_objCType), "r*16@0:8"},
        {"isEqualToNumber:", IMP(nu_isEqualToNumber), "B24@0:8@16"},
        {"isEqual:", IMP(nu_isEqual), "B24@0:8@16"},
        {"hash", IMP(nu_hash), "Q16@0:8"},
        {"compare:", IMP(nu_compare), "q24@0:8@16"},
        {"copy", IMP(nu_copy), "@16@0:8"},
        {"copyWithZone:", IMP(nu_copyWithZone), "@24@0:8^v16"},
        {"getValue:", IMP(nu_getValue), "v24@0:8^v16"},
        {"initWithChar:", IMP(nu_initWithChar), "@24@0:8c16"},
        {"initWithUnsignedChar:", IMP(nu_initWithUnsignedChar), "@24@0:8C16"},
        {"initWithShort:", IMP(nu_initWithShort), "@24@0:8s16"},
        {"initWithUnsignedShort:", IMP(nu_initWithUnsignedShort), "@24@0:8S16"},
        {"initWithInt:", IMP(nu_initWithInt), "@24@0:8i16"},
        {"initWithUnsignedInt:", IMP(nu_initWithUnsignedInt), "@24@0:8I16"},
        {"initWithLong:", IMP(nu_initWithLong), "@24@0:8q16"},
        {"initWithLongLong:", IMP(nu_initWithLong), "@24@0:8q16"},
        {"initWithInteger:", IMP(nu_initWithLong), "@24@0:8q16"},
        {"initWithUnsignedLong:", IMP(nu_initWithUnsignedLong), "@24@0:8Q16"},
        {"initWithUnsignedLongLong:", IMP(nu_initWithUnsignedLong), "@24@0:8Q16"},
        {"initWithUnsignedInteger:", IMP(nu_initWithUnsignedLong), "@24@0:8Q16"},
        {"initWithBool:", IMP(nu_initWithBool), "@24@0:8B16"},
        {"initWithFloat:", IMP(nu_initWithFloat), "@24@0:8f16"},
        {"initWithDouble:", IMP(nu_initWithDouble), "@24@0:8d16"});
    FND_CLASS_METHODS(g_number,
        {"numberWithChar:", IMP(nc_numberWithChar), "@24@0:8c16"},
        {"numberWithUnsignedChar:", IMP(nc_numberWithUnsignedChar), "@24@0:8C16"},
        {"numberWithShort:", IMP(nc_numberWithShort), "@24@0:8s16"},
        {"numberWithUnsignedShort:", IMP(nc_numberWithUnsignedShort), "@24@0:8S16"},
        {"numberWithInt:", IMP(nc_numberWithInt), "@24@0:8i16"},
        {"numberWithUnsignedInt:", IMP(nc_numberWithUnsignedInt), "@24@0:8I16"},
        {"numberWithLong:", IMP(nc_numberWithLong), "@24@0:8q16"},
        {"numberWithLongLong:", IMP(nc_numberWithLong), "@24@0:8q16"},
        {"numberWithInteger:", IMP(nc_numberWithLong), "@24@0:8q16"},
        {"numberWithUnsignedLong:", IMP(nc_numberWithUnsignedLong), "@24@0:8Q16"},
        {"numberWithUnsignedLongLong:", IMP(nc_numberWithUnsignedLong), "@24@0:8Q16"},
        {"numberWithUnsignedInteger:", IMP(nc_numberWithUnsignedLong), "@24@0:8Q16"},
        {"numberWithBool:", IMP(nc_numberWithBool), "@24@0:8B16"},
        {"numberWithFloat:", IMP(nc_numberWithFloat), "@24@0:8f16"},
        {"numberWithDouble:", IMP(nc_numberWithDouble), "@24@0:8d16"});
    /* Static booleans and empty collections the guest binds to by symbol. */
    kCFBooleanTrue_storage = {}; kCFBooleanTrue_storage.base.isa = g_boolean; kCFBooleanTrue_storage.type = 'B'; kCFBooleanTrue_storage.v.i = 1;
    kCFBooleanFalse_storage = {}; kCFBooleanFalse_storage.base.isa = g_boolean; kCFBooleanFalse_storage.type = 'B'; kCFBooleanFalse_storage.v.i = 0;
    kCFBooleanTrue_ptr = reinterpret_cast<id>(&kCFBooleanTrue_storage);
    kCFBooleanFalse_ptr = reinterpret_cast<id>(&kCFBooleanFalse_storage);
    new (&NSArray0_storage) array_object; NSArray0_storage.base.isa = g_carray;
    new (&NSDictionary0_storage) dict_object; NSDictionary0_storage.base.isa = g_cdict;
    static auto immortal = [](id self, SEL) -> std::uint64_t { return ret(self); };
    static auto immortal_v = [](id, SEL) -> std::uint64_t { return 0; };
    for (Class c : {g_boolean}) {
        mm_objc_add_method(c, "retain", IMP(+immortal), "@16@0:8");
        mm_objc_add_method(c, "release", IMP(+immortal_v), "v16@0:8");
        mm_objc_add_method(c, "autorelease", IMP(+immortal), "@16@0:8");
    }

    g_null = mm_objc_define_class("NSNull", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSNull), FND_META_REF(NSNull));
    kCFNull_ptr = mm_objc_alloc(g_null, 0);
    FND_CLASS_METHODS(g_null, {"null", IMP(null_null), "@16@0:8"});
    FND_METHODS(g_null, {"description", IMP(null_description), "@16@0:8"}, {"copyWithZone:", IMP(nu_copyWithZone), "@24@0:8^v16"},
                {"retain", IMP(+immortal), "@16@0:8"}, {"release", IMP(+immortal_v), "v16@0:8"}, {"autorelease", IMP(+immortal), "@16@0:8"});
}

/* ---- CF C API ---------------------------------------------------------------- */

extern "C" {

CFTypeID CFArrayGetTypeID_() DARWIN(CFArrayGetTypeID);
CFTypeID CFArrayGetTypeID_() { return ret(g_array); }
id CFArrayCreate_(void *, const id *values, CFIndex n, const void *) DARWIN(CFArrayCreate);
id CFArrayCreate_(void *, const id *values, CFIndex n, const void *) { return array_create(values, n, false); }
id CFArrayCreateMutable_(void *, CFIndex, const void *) DARWIN(CFArrayCreateMutable);
id CFArrayCreateMutable_(void *, CFIndex, const void *) { return array_create(nullptr, 0, true); }
id CFArrayCreateCopy_(void *, id a) DARWIN(CFArrayCreateCopy);
id CFArrayCreateCopy_(void *, id a) { return obj(a_copy(a, nullptr)); }
id CFArrayCreateMutableCopy_(void *, CFIndex, id a) DARWIN(CFArrayCreateMutableCopy);
id CFArrayCreateMutableCopy_(void *, CFIndex, id a) { return obj(a_mutableCopy(a, nullptr)); }
CFIndex CFArrayGetCount_(id a) DARWIN(CFArrayGetCount);
CFIndex CFArrayGetCount_(id a) { return static_cast<CFIndex>(arr(a)->items.size()); }
id CFArrayGetValueAtIndex_(id a, CFIndex i) DARWIN(CFArrayGetValueAtIndex);
id CFArrayGetValueAtIndex_(id a, CFIndex i) { return obj(a_objectAtIndex(a, nullptr, static_cast<std::uint64_t>(i))); }
void CFArrayGetValues_(id a, CFIndex loc, CFIndex len, id *out) DARWIN(CFArrayGetValues);
void CFArrayGetValues_(id a, CFIndex loc, CFIndex len, id *out) { a_getObjectsRange(a, nullptr, out, static_cast<std::uint64_t>(loc), static_cast<std::uint64_t>(len)); }
void CFArrayAppendValue_(id a, id v) DARWIN(CFArrayAppendValue);
void CFArrayAppendValue_(id a, id v) { ma_addObject(a, nullptr, v); }
void CFArrayInsertValueAtIndex_(id a, CFIndex i, id v) DARWIN(CFArrayInsertValueAtIndex);
void CFArrayInsertValueAtIndex_(id a, CFIndex i, id v) { ma_insertObjectAtIndex(a, nullptr, v, static_cast<std::uint64_t>(i)); }
void CFArrayRemoveValueAtIndex_(id a, CFIndex i) DARWIN(CFArrayRemoveValueAtIndex);
void CFArrayRemoveValueAtIndex_(id a, CFIndex i) { ma_removeObjectAtIndex(a, nullptr, static_cast<std::uint64_t>(i)); }
void CFArrayRemoveAllValues_(id a) DARWIN(CFArrayRemoveAllValues);
void CFArrayRemoveAllValues_(id a) { ma_removeAllObjects(a, nullptr); }
void CFArraySetValueAtIndex_(id a, CFIndex i, id v) DARWIN(CFArraySetValueAtIndex);
void CFArraySetValueAtIndex_(id a, CFIndex i, id v) { ma_replaceObjectAtIndex(a, nullptr, static_cast<std::uint64_t>(i), v); }
void CFArrayAppendArray_(id a, id o, CFIndex loc, CFIndex len) DARWIN(CFArrayAppendArray);
void CFArrayAppendArray_(id a, id o, CFIndex loc, CFIndex len) { for (CFIndex i = loc; i < loc + len; i++) ma_addObject(a, nullptr, arr(o)->items[static_cast<std::size_t>(i)]); }
int CFArrayContainsValue_(id a, CFIndex, CFIndex, id v) DARWIN(CFArrayContainsValue);
int CFArrayContainsValue_(id a, CFIndex, CFIndex, id v) { return static_cast<int>(a_containsObject(a, nullptr, v)); }
CFIndex CFArrayGetFirstIndexOfValue_(id a, CFIndex loc, CFIndex len, id v) DARWIN(CFArrayGetFirstIndexOfValue);
CFIndex CFArrayGetFirstIndexOfValue_(id a, CFIndex loc, CFIndex len, id v) { auto &items = arr(a)->items; for (CFIndex i = loc; i < loc + len && i < static_cast<CFIndex>(items.size()); i++) if (objects_equal(items[static_cast<std::size_t>(i)], v)) return i; return kCFNotFound; }
void CFArrayApplyFunction_(id a, CFIndex loc, CFIndex len, void (*fn)(id, void *), void *ctx) DARWIN(CFArrayApplyFunction);
void CFArrayApplyFunction_(id a, CFIndex loc, CFIndex len, void (*fn)(id, void *), void *ctx) {
    auto items = arr(a)->items;
    for (CFIndex i = loc; i < loc + len && i < static_cast<CFIndex>(items.size()); i++) {
        const std::uint64_t args[2] = {ret(items[static_cast<std::size_t>(i)]), ret(ctx)};
        if (api()->is_guest_code(reinterpret_cast<std::uintptr_t>(fn))) api()->call_guest(reinterpret_cast<std::uintptr_t>(fn), args, 2);
        else fn(items[static_cast<std::size_t>(i)], ctx);
    }
}

CFTypeID CFDictionaryGetTypeID_() DARWIN(CFDictionaryGetTypeID);
CFTypeID CFDictionaryGetTypeID_() { return ret(g_dict); }
id CFDictionaryCreate_(void *, const id *keys, const id *values, CFIndex n, const void *, const void *) DARWIN(CFDictionaryCreate);
id CFDictionaryCreate_(void *, const id *keys, const id *values, CFIndex n, const void *, const void *) { id d = dictionary_create(false); for (CFIndex i = 0; i < n; i++) dictionary_set(d, keys[i], values[i]); return d; }
id CFDictionaryCreateMutable_(void *, CFIndex, const void *, const void *) DARWIN(CFDictionaryCreateMutable);
id CFDictionaryCreateMutable_(void *, CFIndex, const void *, const void *) { return dictionary_create(true); }
id CFDictionaryCreateCopy_(void *, id d) DARWIN(CFDictionaryCreateCopy);
id CFDictionaryCreateCopy_(void *, id d) { return obj(d_copy(d, nullptr)); }
id CFDictionaryCreateMutableCopy_(void *, CFIndex, id d) DARWIN(CFDictionaryCreateMutableCopy);
id CFDictionaryCreateMutableCopy_(void *, CFIndex, id d) { return obj(d_mutableCopy(d, nullptr)); }
CFIndex CFDictionaryGetCount_(id d) DARWIN(CFDictionaryGetCount);
CFIndex CFDictionaryGetCount_(id d) { return dictionary_count(d); }
id CFDictionaryGetValue_(id d, id k) DARWIN(CFDictionaryGetValue);
id CFDictionaryGetValue_(id d, id k) { return d && k ? dictionary_get(d, k) : nullptr; }
int CFDictionaryGetValueIfPresent_(id d, id k, id *out) DARWIN(CFDictionaryGetValueIfPresent);
int CFDictionaryGetValueIfPresent_(id d, id k, id *out) { id v = CFDictionaryGetValue_(d, k); if (!v) return 0; if (out) *out = v; return 1; }
int CFDictionaryContainsKey_(id d, id k) DARWIN(CFDictionaryContainsKey);
int CFDictionaryContainsKey_(id d, id k) { return CFDictionaryGetValue_(d, k) != nullptr; }
void CFDictionarySetValue_(id d, id k, id v) DARWIN(CFDictionarySetValue);
void CFDictionarySetValue_(id d, id k, id v) { dictionary_set(d, k, v); }
void CFDictionaryAddValue_(id d, id k, id v) DARWIN(CFDictionaryAddValue);
void CFDictionaryAddValue_(id d, id k, id v) { if (!dictionary_get(d, k)) dictionary_set(d, k, v); }
void CFDictionaryReplaceValue_(id d, id k, id v) DARWIN(CFDictionaryReplaceValue);
void CFDictionaryReplaceValue_(id d, id k, id v) { if (dictionary_get(d, k)) dictionary_set(d, k, v); }
void CFDictionaryRemoveValue_(id d, id k) DARWIN(CFDictionaryRemoveValue);
void CFDictionaryRemoveValue_(id d, id k) { dictionary_remove(d, k); }
void CFDictionaryRemoveAllValues_(id d) DARWIN(CFDictionaryRemoveAllValues);
void CFDictionaryRemoveAllValues_(id d) { auto keys = dict(d)->order; for (id k : keys) dictionary_remove(d, k); }
void CFDictionaryGetKeysAndValues_(id d, id *keys, id *values) DARWIN(CFDictionaryGetKeysAndValues);
void CFDictionaryGetKeysAndValues_(id d, id *keys, id *values) { std::size_t i = 0; for (id k : dict(d)->order) { if (keys) keys[i] = k; if (values) values[i] = dict(d)->map[k]; i++; } }
void CFDictionaryApplyFunction_(id d, void (*fn)(id, id, void *), void *ctx) DARWIN(CFDictionaryApplyFunction);
void CFDictionaryApplyFunction_(id d, void (*fn)(id, id, void *), void *ctx) {
    auto order = dict(d)->order;
    for (id k : order) {
        id v = dict(d)->map[k];
        const std::uint64_t args[3] = {ret(k), ret(v), ret(ctx)};
        if (api()->is_guest_code(reinterpret_cast<std::uintptr_t>(fn))) api()->call_guest(reinterpret_cast<std::uintptr_t>(fn), args, 3);
        else fn(k, v, ctx);
    }
}

CFTypeID CFSetGetTypeID_() DARWIN(CFSetGetTypeID);
CFTypeID CFSetGetTypeID_() { return ret(g_set); }
id CFSetCreate_(void *, const id *values, CFIndex n, const void *) DARWIN(CFSetCreate);
id CFSetCreate_(void *, const id *values, CFIndex n, const void *) { id s = obj(send(reinterpret_cast<id>(g_set), "alloc")); return obj(s_initWithObjectsCount(s, nullptr, values, static_cast<std::uint64_t>(n))); }
id CFSetCreateMutable_(void *, CFIndex, const void *) DARWIN(CFSetCreateMutable);
id CFSetCreateMutable_(void *, CFIndex, const void *) { return obj(send(reinterpret_cast<id>(g_mset), "alloc")); }
CFIndex CFSetGetCount_(id s) DARWIN(CFSetGetCount);
CFIndex CFSetGetCount_(id s) { return static_cast<CFIndex>(set(s)->items.size()); }
int CFSetContainsValue_(id s, id v) DARWIN(CFSetContainsValue);
int CFSetContainsValue_(id s, id v) { return set_contains(set(s), v); }
void CFSetAddValue_(id s, id v) DARWIN(CFSetAddValue);
void CFSetAddValue_(id s, id v) { ms_addObject(s, nullptr, v); }
void CFSetRemoveValue_(id s, id v) DARWIN(CFSetRemoveValue);
void CFSetRemoveValue_(id s, id v) { ms_removeObject(s, nullptr, v); }
void CFSetGetValues_(id s, id *out) DARWIN(CFSetGetValues);
void CFSetGetValues_(id s, id *out) { std::size_t i = 0; for (id e : set(s)->items) out[i++] = e; }

CFTypeID CFDataGetTypeID_() DARWIN(CFDataGetTypeID);
CFTypeID CFDataGetTypeID_() { return ret(g_data); }
id CFDataCreate_(void *, const std::uint8_t *b, CFIndex n) DARWIN(CFDataCreate);
id CFDataCreate_(void *, const std::uint8_t *b, CFIndex n) { return data_create(b, n, false); }
id CFDataCreateWithBytesNoCopy_(void *, const std::uint8_t *b, CFIndex n, void *) DARWIN(CFDataCreateWithBytesNoCopy);
id CFDataCreateWithBytesNoCopy_(void *, const std::uint8_t *b, CFIndex n, void *) { return data_create(b, n, false); }
id CFDataCreateMutable_(void *, CFIndex) DARWIN(CFDataCreateMutable);
id CFDataCreateMutable_(void *, CFIndex) { return data_create(nullptr, 0, true); }
id CFDataCreateCopy_(void *, id d) DARWIN(CFDataCreateCopy);
id CFDataCreateCopy_(void *, id d) { return obj(n_copy(d, nullptr)); }
id CFDataCreateMutableCopy_(void *, CFIndex, id d) DARWIN(CFDataCreateMutableCopy);
id CFDataCreateMutableCopy_(void *, CFIndex, id d) { return obj(n_mutableCopy(d, nullptr)); }
CFIndex CFDataGetLength_(id d) DARWIN(CFDataGetLength);
CFIndex CFDataGetLength_(id d) { return static_cast<CFIndex>(dat(d)->bytes.size()); }
const std::uint8_t *CFDataGetBytePtr_(id d) DARWIN(CFDataGetBytePtr);
const std::uint8_t *CFDataGetBytePtr_(id d) { return reinterpret_cast<const std::uint8_t *>(dat(d)->bytes.data()); }
std::uint8_t *CFDataGetMutableBytePtr_(id d) DARWIN(CFDataGetMutableBytePtr);
std::uint8_t *CFDataGetMutableBytePtr_(id d) { return reinterpret_cast<std::uint8_t *>(dat(d)->bytes.data()); }
void CFDataGetBytes_(id d, CFIndex loc, CFIndex len, std::uint8_t *out) DARWIN(CFDataGetBytes);
void CFDataGetBytes_(id d, CFIndex loc, CFIndex len, std::uint8_t *out) { n_getBytesRange(d, nullptr, out, static_cast<std::uint64_t>(loc), static_cast<std::uint64_t>(len)); }
void CFDataAppendBytes_(id d, const std::uint8_t *b, CFIndex n) DARWIN(CFDataAppendBytes);
void CFDataAppendBytes_(id d, const std::uint8_t *b, CFIndex n) { mn_appendBytesLength(d, nullptr, b, static_cast<std::uint64_t>(n)); }
void CFDataSetLength_(id d, CFIndex n) DARWIN(CFDataSetLength);
void CFDataSetLength_(id d, CFIndex n) { mn_setLength(d, nullptr, static_cast<std::uint64_t>(n)); }
void CFDataIncreaseLength_(id d, CFIndex n) DARWIN(CFDataIncreaseLength);
void CFDataIncreaseLength_(id d, CFIndex n) { mn_increaseLengthBy(d, nullptr, static_cast<std::uint64_t>(n)); }
void CFDataReplaceBytes_(id d, CFIndex loc, CFIndex len, const std::uint8_t *b, CFIndex n) DARWIN(CFDataReplaceBytes);
void CFDataReplaceBytes_(id d, CFIndex loc, CFIndex len, const std::uint8_t *b, CFIndex n) { auto &s = dat(d)->bytes; s.replace(static_cast<std::size_t>(loc), static_cast<std::size_t>(len), reinterpret_cast<const char *>(b), static_cast<std::size_t>(n)); }
void CFDataDeleteBytes_(id d, CFIndex loc, CFIndex len) DARWIN(CFDataDeleteBytes);
void CFDataDeleteBytes_(id d, CFIndex loc, CFIndex len) { dat(d)->bytes.erase(static_cast<std::size_t>(loc), static_cast<std::size_t>(len)); }

CFTypeID CFNumberGetTypeID_() DARWIN(CFNumberGetTypeID);
CFTypeID CFNumberGetTypeID_() { return ret(g_number); }
CFTypeID CFBooleanGetTypeID_() DARWIN(CFBooleanGetTypeID);
CFTypeID CFBooleanGetTypeID_() { return ret(g_boolean); }
int CFBooleanGetValue_(id b) DARWIN(CFBooleanGetValue);
int CFBooleanGetValue_(id b) { return b == kCFBooleanTrue_ptr || (is_number(b) && num(b)->v.i != 0); }
/* CFNumberType: 1 SInt8 2 SInt16 3 SInt32 4 SInt64 5 Float32 6 Float64 7 char 8 short 9 int 10 long 11 longlong 12 float 13 double 14 CFIndex 15 NSInteger 16 CGFloat */
id CFNumberCreate_(void *, std::int64_t type, const void *p) DARWIN(CFNumberCreate);
id CFNumberCreate_(void *, std::int64_t type, const void *p) {
    switch (type) {
    case 1: case 7: { std::int8_t v; std::memcpy(&v, p, 1); return number_new('c', v, 0); }
    case 2: case 8: { std::int16_t v; std::memcpy(&v, p, 2); return number_new('s', v, 0); }
    case 3: case 9: { std::int32_t v; std::memcpy(&v, p, 4); return number_new('i', v, 0); }
    case 5: case 12: { float v; std::memcpy(&v, p, 4); return number_new('f', 0, v); }
    case 6: case 13: case 16: { double v; std::memcpy(&v, p, 8); return number_new('d', 0, v); }
    default: { std::int64_t v; std::memcpy(&v, p, 8); return number_new('q', v, 0); }
    }
}
int CFNumberGetValue_(id n, std::int64_t type, void *out) DARWIN(CFNumberGetValue);
int CFNumberGetValue_(id n, std::int64_t type, void *out) {
    number_object *o = num(n);
    switch (type) {
    case 1: case 7: { const std::int8_t v = static_cast<std::int8_t>(num_int(o)); std::memcpy(out, &v, 1); return num_int(o) == v; }
    case 2: case 8: { const std::int16_t v = static_cast<std::int16_t>(num_int(o)); std::memcpy(out, &v, 2); return num_int(o) == v; }
    case 3: case 9: { const std::int32_t v = static_cast<std::int32_t>(num_int(o)); std::memcpy(out, &v, 4); return num_int(o) == v; }
    case 5: case 12: { const float v = static_cast<float>(num_double(o)); std::memcpy(out, &v, 4); return 1; }
    case 6: case 13: case 16: { const double v = num_double(o); std::memcpy(out, &v, 8); return 1; }
    default: { const std::int64_t v = num_int(o); std::memcpy(out, &v, 8); return !num_is_float(o) || std::floor(o->v.d) == o->v.d; }
    }
}
std::int64_t CFNumberGetType_(id n) DARWIN(CFNumberGetType);
std::int64_t CFNumberGetType_(id n) {
    switch (num(n)->type) { case 'c': case 'C': case 'B': return 1; case 's': case 'S': return 2; case 'i': case 'I': return 3; case 'f': return 5; case 'd': return 6; default: return 4; }
}
int CFNumberIsFloatType_(id n) DARWIN(CFNumberIsFloatType);
int CFNumberIsFloatType_(id n) { return num_is_float(num(n)); }
std::int64_t CFNumberCompare_(id a, id b, void *) DARWIN(CFNumberCompare);
std::int64_t CFNumberCompare_(id a, id b, void *) { return static_cast<std::int64_t>(nu_compare(a, nullptr, b)); }
CFTypeID CFNullGetTypeID_() DARWIN(CFNullGetTypeID);
CFTypeID CFNullGetTypeID_() { return ret(g_null); }

} // extern "C"

} // namespace fnd
