#include "foundation.h"
#include <algorithm>
#include <utility>

namespace fnd {
FND_CLASS(NSIndexSet);
FND_CLASS(NSMutableIndexSet);
namespace {
using index = std::uint64_t;
using interval = std::pair<index, index>; // [first, end), sorted and disjoint.
struct index_set_object {
    mm_objc_object base;
    std::vector<interval> ranges;
};
Class index_set_class, mutable_index_set_class;
auto &ranges(id self) { return as<index_set_object>(self)->ranges; }
void add_range(id self, index first, index length) {
    if (!length || first >= NSNotFound || length > NSNotFound - first) return;
    interval next{first, first + length};
    std::vector<interval> result;
    for (auto current : ranges(self)) {
        if (current.second < next.first) result.push_back(current);
        else if (next.second < current.first) { result.push_back(next); next = current; }
        else { next.first = std::min(next.first,current.first); next.second = std::max(next.second,current.second); }
    }
    result.push_back(next);
    ranges(self) = std::move(result);
}
std::uint64_t init_range(id self, SEL, index first, index length) {
    ranges(self).clear(); add_range(self,first,length); return ret(self);
}
std::uint64_t init_index(id self, SEL, index value) { return init_range(self,nullptr,value,1); }
std::uint64_t init_set(id self, SEL, id other) { ranges(self) = other ? ranges(other) : std::vector<interval>{}; return ret(self); }
std::uint64_t factory(id klass, SEL) { return ret(autorelease(obj(send(klass,"new")))); }
std::uint64_t factory_range(id klass, SEL, index first, index length) {
    id value = obj(send(klass,"alloc")); init_range(value,nullptr,first,length); return ret(autorelease(value));
}
std::uint64_t factory_index(id klass, SEL, index value) { return factory_range(klass,nullptr,value,1); }
std::uint64_t count(id self, SEL) { index total=0; for(auto r:ranges(self)) total+=r.second-r.first; return total; }
std::uint64_t first(id self, SEL) { return ranges(self).empty() ? NSNotFound : ranges(self).front().first; }
std::uint64_t last(id self, SEL) { return ranges(self).empty() ? NSNotFound : ranges(self).back().second-1; }
std::uint64_t contains(id self, SEL, index value) {
    for (auto r:ranges(self)) { if(value<r.first) return 0; if(value<r.second) return 1; } return 0;
}
std::uint64_t at_least(id self, SEL, index value) {
    for(auto r:ranges(self)) if(value<r.second) return std::max(value,r.first); return NSNotFound;
}
std::uint64_t greater(id self, SEL, index value) { return value>=NSNotFound-1 ? NSNotFound : at_least(self,nullptr,value+1); }
std::uint64_t at_most(id self, SEL, index value) {
    for(auto it=ranges(self).rbegin();it!=ranges(self).rend();++it)
        if(value>=it->first) return std::min(value,it->second-1);
    return NSNotFound;
}
std::uint64_t less(id self, SEL, index value) { return value ? at_most(self,nullptr,value-1) : NSNotFound; }
std::uint64_t copy_to(id self, Class klass) {
    auto *result=make<index_set_object>(klass); result->ranges=ranges(self); return ret(result);
}
std::uint64_t copy(id self, SEL) { return mm_objc_object_class(self)==index_set_class ? ret(retain(self)) : copy_to(self,index_set_class); }
std::uint64_t mutable_copy(id self, SEL) { return copy_to(self,mutable_index_set_class); }
std::uint64_t equal(id self, SEL, id other) {
    return other && is_kind(other,index_set_class) && ranges(self)==ranges(other);
}
std::uint64_t add(id self, SEL, index value) { add_range(self,value,1); return 0; }
std::uint64_t add_indexes(id self, SEL, id other) {
    if(other) for(auto r:std::vector<interval>(ranges(other))) add_range(self,r.first,r.second-r.first); return 0;
}
std::uint64_t add_indexes_range(id self, SEL, index first, index length) { add_range(self,first,length); return 0; }
std::uint64_t remove_range(id self, SEL, index first, index length) {
    if(!length || first>=NSNotFound) return 0;
    const index end=first+std::min(length,NSNotFound-first);
    std::vector<interval> result;
    for(auto r:ranges(self)) {
        if(r.second<=first || r.first>=end) result.push_back(r);
        else { if(r.first<first) result.emplace_back(r.first,first); if(r.second>end) result.emplace_back(end,r.second); }
    }
    ranges(self)=std::move(result); return 0;
}
std::uint64_t remove(id self, SEL, index value) { return remove_range(self,nullptr,value,1); }
std::uint64_t clear(id self, SEL) { ranges(self).clear(); return 0; }
}
void register_index_sets() {
    index_set_class=mm_objc_define_class("NSIndexSet",cls("NSObject"),sizeof(index_set_object),FND_CLASS_REF(NSIndexSet),FND_META_REF(NSIndexSet));
    mutable_index_set_class=mm_objc_define_class("NSMutableIndexSet",index_set_class,sizeof(index_set_object),FND_CLASS_REF(NSMutableIndexSet),FND_META_REF(NSMutableIndexSet));
    register_lifecycle<index_set_object>(index_set_class);
    FND_CLASS_METHODS(index_set_class,
        {"indexSet",IMP(factory),"@16@0:8"}, {"indexSetWithIndex:",IMP(factory_index),"@24@0:8Q16"},
        {"indexSetWithIndexesInRange:",IMP(factory_range),"@32@0:8{_NSRange=QQ}16"});
    FND_METHODS(index_set_class,
        {"initWithIndex:",IMP(init_index),"@24@0:8Q16"}, {"initWithIndexesInRange:",IMP(init_range),"@32@0:8{_NSRange=QQ}16"},
        {"initWithIndexSet:",IMP(init_set),"@24@0:8@16"}, {"count",IMP(count),"Q16@0:8"},
        {"hash",IMP(count),"Q16@0:8"},
        {"firstIndex",IMP(first),"Q16@0:8"}, {"lastIndex",IMP(last),"Q16@0:8"},
        {"containsIndex:",IMP(contains),"B24@0:8Q16"}, {"indexGreaterThanIndex:",IMP(greater),"Q24@0:8Q16"},
        {"indexLessThanIndex:",IMP(less),"Q24@0:8Q16"}, {"indexGreaterThanOrEqualToIndex:",IMP(at_least),"Q24@0:8Q16"},
        {"indexLessThanOrEqualToIndex:",IMP(at_most),"Q24@0:8Q16"},
        {"copy",IMP(copy),"@16@0:8"}, {"copyWithZone:",IMP(copy),"@24@0:8^v16"},
        {"mutableCopy",IMP(mutable_copy),"@16@0:8"}, {"mutableCopyWithZone:",IMP(mutable_copy),"@24@0:8^v16"},
        {"isEqual:",IMP(equal),"B24@0:8@16"}, {"isEqualToIndexSet:",IMP(equal),"B24@0:8@16"});
    FND_METHODS(mutable_index_set_class,
        {"addIndex:",IMP(add),"v24@0:8Q16"}, {"addIndexes:",IMP(add_indexes),"v24@0:8@16"},
        {"addIndexesInRange:",IMP(add_indexes_range),"v32@0:8{_NSRange=QQ}16"},
        {"removeIndex:",IMP(remove),"v24@0:8Q16"}, {"removeAllIndexes",IMP(clear),"v16@0:8"},
        {"removeIndexesInRange:",IMP(remove_range),"v32@0:8{_NSRange=QQ}16"});
}
} // namespace fnd
