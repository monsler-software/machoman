#include "foundation.h"
#include <algorithm>
#include <cstdio>

namespace fnd {
FND_CLASS(NSAttributedString);
FND_CLASS(NSMutableAttributedString);
FND_CLASS(NSFormatter);
FND_CLASS(NSNumberFormatter);
namespace {
struct attributed {
    mm_objc_object base;
    id text = nullptr;
    struct span { id name, value; std::uint64_t location, length; };
    std::vector<span> attributes;
    ~attributed(){release(text);for(auto &s:attributes){release(s.name);release(s.value);}}
};
std::uint64_t attributed_init(id self, SEL, id text) {
    auto *a=as<attributed>(self);id copy=obj(send(text,"copy"));release(a->text);a->text=copy;return ret(self);
}
std::uint64_t attributed_text(id self,SEL){return ret(as<attributed>(self)->text);}
std::uint64_t attributed_length(id self,SEL){return send(as<attributed>(self)->text,"length");}
std::uint64_t attributed_attribute(id self,SEL,id name,id value,std::uint64_t loc,std::uint64_t length){
    const auto size=send(as<attributed>(self)->text,"length");
    if(name&&value&&loc<=size&&length<=size-loc)as<attributed>(self)->attributes.push_back({retain(name),retain(value),loc,length});return 0;
}
struct formatter {
    mm_objc_object base;
    id decimal = nullptr;
    std::uint64_t minimum = 0, maximum = 3;
    ~formatter(){release(decimal);}
};
std::uint64_t decimal(id self,SEL){return ret(as<formatter>(self)->decimal?as<formatter>(self)->decimal:constant_string("."));}
std::uint64_t set_decimal(id self,SEL,id value){auto *f=as<formatter>(self);id copy=obj(send(value,"copy"));release(f->decimal);f->decimal=copy;return 0;}
std::uint64_t format_number(id self,SEL,id value){
    if(!value||!is_number(value))return 0;
    const auto *f=as<formatter>(self);const unsigned precision=std::min<std::uint64_t>(20,f->maximum);
    char buffer[384];std::snprintf(buffer,sizeof buffer,"%.*f",precision,number_double(value));std::string text(buffer);
    auto dot=text.find('.');if(dot!=std::string::npos){while(text.size()>dot+1+std::min<std::uint64_t>(precision,f->minimum)&&text.back()=='0')text.pop_back();if(text.back()=='.')text.pop_back();else text.replace(dot,1,to_utf8(obj(decimal(self,nullptr))));}
    return ret(autorelease(string_from_utf8(text)));
}
}
void register_formatting(){
    Class attr=mm_objc_define_class("NSAttributedString",cls("NSObject"),sizeof(attributed),FND_CLASS_REF(NSAttributedString),FND_META_REF(NSAttributedString));
    Class mut=mm_objc_define_class("NSMutableAttributedString",attr,sizeof(attributed),FND_CLASS_REF(NSMutableAttributedString),FND_META_REF(NSMutableAttributedString));
    register_lifecycle<attributed>(attr);register_lifecycle<attributed>(mut);
    FND_METHODS(attr,{"initWithString:",IMP(attributed_init),"@24@0:8@16"},{"string",IMP(attributed_text),"@16@0:8"},{"length",IMP(attributed_length),"Q16@0:8"});
    FND_METHODS(mut,{"addAttribute:value:range:",IMP(attributed_attribute),"v48@0:8@16@24{_NSRange=QQ}32"});
    Class base=mm_objc_define_class("NSFormatter",cls("NSObject"),sizeof(formatter),FND_CLASS_REF(NSFormatter),FND_META_REF(NSFormatter));
    Class number=mm_objc_define_class("NSNumberFormatter",base,sizeof(formatter),FND_CLASS_REF(NSNumberFormatter),FND_META_REF(NSNumberFormatter));
    register_lifecycle<formatter>(base);register_lifecycle<formatter>(number);
    static auto localizes=[](id,SEL,std::uint64_t)->std::uint64_t{return 0;};
    static auto min=[](id s,SEL,std::uint64_t n)->std::uint64_t{as<formatter>(s)->minimum=n;return 0;};
    static auto max=[](id s,SEL,std::uint64_t n)->std::uint64_t{as<formatter>(s)->maximum=n;return 0;};
    FND_METHODS(number,{"decimalSeparator",IMP(decimal),"@16@0:8"},{"setDecimalSeparator:",IMP(set_decimal),"v24@0:8@16"},
        {"setLocalizesFormat:",IMP(+localizes),"v24@0:8B16"},{"setMinimumFractionDigits:",IMP(+min),"v24@0:8Q16"},{"setMaximumFractionDigits:",IMP(+max),"v24@0:8Q16"},
        {"stringFromNumber:",IMP(format_number),"@24@0:8@16"},{"stringForObjectValue:",IMP(format_number),"@24@0:8@16"});
}
}
