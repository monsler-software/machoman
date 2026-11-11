/* Included inside libobjc's anonymous namespace. Cold resolution creates
 * a descriptor once; normal sends use the existing method cache and apply
 * the ARM64 return plan directly, avoiding a second host trampoline. */
bool stub_family(std::string_view name, std::string_view prefix) {
    return name.starts_with(prefix) && (name.size() == prefix.size() ||
        !std::islower(static_cast<unsigned char>(name[prefix.size()])));
}

std::string default_property(std::string_view sel, bool &setter) {
    setter = sel.starts_with("set") && sel.ends_with(':') &&
        sel.size() > 4 && sel.find(':') == sel.size() - 1;
    std::string key(setter ? sel.substr(3, sel.size() - 4) : sel);
    if (setter) key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
    return key;
}

const char *protocol_signature(protocol_t *p, SEL sel, bool meta, unsigned depth = 0) {
    if (!p || depth > 32) return nullptr;
    for (auto *list : {meta ? p->classMethods : p->instanceMethods,
                       meta ? p->optionalClassMethods : p->optionalInstanceMethods}) {
        if (!list) continue;
        /* Protocol descriptions have no IMP and use big method entries. */
        const auto stride = list->entsize_flags & 0xFFFCu;
        if (list->entsize_flags & 0x80000000u || stride < sizeof(method_big)) continue;
        auto *base = reinterpret_cast<const std::uint8_t *>(list + 1);
        for (std::uint32_t i = 0; i < list->count; ++i) {
            auto *method = reinterpret_cast<const method_big *>(base + i * stride);
            if (sel_intern(method->name) == sel) return method->types;
        }
    }
    if (p->protocols) {
        auto **list = reinterpret_cast<protocol_t **>(p->protocols + 1);
        for (std::uintptr_t i = 0; i < p->protocols->count; ++i)
            if (auto *types = protocol_signature(list[i], sel, meta, depth + 1)) return types;
    }
    return nullptr;
}

struct property_list { std::uint32_t stride, count; };
struct property_entry { const char *name, *attributes; };
bool property_signature(const void *raw, std::string_view selector, std::string &encoding, std::string &key) {
    if (!raw) return false;
    auto *list = static_cast<const property_list *>(raw);
    if (list->stride < sizeof(property_entry)) return false;
    auto *base = reinterpret_cast<const std::uint8_t *>(list + 1);
    for (std::uint32_t i = 0; i < list->count; ++i) {
        auto *property = reinterpret_cast<const property_entry *>(base + i * list->stride);
        std::string_view attributes(property->attributes);
        if (!attributes.starts_with('T')) continue;
        auto remaining = attributes.substr(1);
        const auto type = stub_abi::parse(remaining);
        if (!type.valid) continue;
        const auto type_size = attributes.size() - 1 - remaining.size();
        std::string_view type_text = attributes.substr(1, type_size);
        std::string getter(property->name), setter("set");
        setter += property->name;
        setter[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter[3])));
        setter += ':';
        bool readonly = false;
        while (remaining.starts_with(',')) {
            remaining.remove_prefix(1);
            const auto comma = remaining.find(',');
            auto attribute = remaining.substr(0, comma);
            if (attribute == "R") readonly = true;
            else if (attribute.starts_with('G')) getter = attribute.substr(1);
            else if (attribute.starts_with('S')) setter = attribute.substr(1);
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma);
        }
        if (selector == getter) encoding = std::string(type_text) + "@:";
        else if (!readonly && selector == setter) encoding = "v@:" + std::string(type_text);
        else continue;
        key = property->name;
        return true;
    }
    return false;
}

/* These shared IMPs also make methodForSelector:/runtime introspection
 * usable. Normal objc_msgSend executes invoke_stub directly. */
mm_cpu_context native_stub_call(id self, SEL sel, std::uint64_t argument) {
    mm_cpu_context *outer = g_api && g_api->current_context ? g_api->current_context() : nullptr;
    mm_cpu_context call{};
    if (outer) call = *outer;
    call.x[0] = reinterpret_cast<std::uintptr_t>(self);
    call.x[1] = reinterpret_cast<std::uintptr_t>(sel);
    call.x[2] = argument;
    std::lock_guard lock(g_lock);
    const method_entry *method = lookup_in(self->isa, sel_intern(sel));
    if (!method) method = resolve_stub(self->isa, sel_intern(sel));
    if (method && method->stub) call.x[0] = invoke_stub(call, *method->stub);
    else call.x[0] = 0;
    if (outer) {
        outer->x[1] = call.x[1];
        std::memcpy(outer->v + 2, call.v + 2, sizeof(call.v[0]) * 2);
    }
    return call;
}
std::uint64_t native_stub_word(id self, SEL sel, std::uint64_t arg) { return native_stub_call(self, sel, arg).x[0]; }
double native_stub_double(id self, SEL sel) {
    auto call = native_stub_call(self, sel, 0); double result;
    std::memcpy(&result, &call.v[0][0], sizeof result); return result;
}
float native_stub_float(id self, SEL sel) {
    auto call = native_stub_call(self, sel, 0); float result;
    std::memcpy(&result, &call.v[0][0], sizeof result); return result;
}
mm_two_doubles native_stub_hfa(id self, SEL sel) {
    auto call = native_stub_call(self, sel, 0); mm_two_doubles result;
    std::memcpy(&result.a, &call.v[0][0], 8); std::memcpy(&result.b, &call.v[1][0], 8); return result;
}

const method_entry *install_stub(Class c, SEL sel, std::string encoding, bool inferred,
                                 bool placeholder, std::string property = {}) {
    auto signature = stub_abi::signature(encoding);
    if (signature.empty()) return nullptr;
    auto method = std::make_unique<dynamic_method>();
    method->encoding = std::move(encoding);
    method->signature = std::move(signature);
    method->inferred = inferred;
    method->placeholder = placeholder;
    const std::string_view name(sel);
    auto key = default_property(name, method->setter);
    const bool custom_property = !property.empty();
    if (custom_property) key = std::move(property);
    method->setter = method->setter && method->signature.size() == 4;
    /* Custom setters supplied by property metadata. */
    if (!method->setter && custom_property && method->signature.size() == 4 && method->signature[0].kind == 'v') method->setter = true;
    method->initializer = stub_family(name, "init") && method->signature[0].kind == '@';
    if (method->setter || (!method->initializer && method->signature.size() == 3 &&
        !stub_family(name, "new") && !stub_family(name, "copy") && !stub_family(name, "mutableCopy"))) method->property = sel_intern_copy(key.c_str());
    auto &result = method->signature[0];
    void *imp = reinterpret_cast<void *>(&native_stub_word);
    if (result.kind == 'd') imp = reinterpret_cast<void *>(&native_stub_double);
    else if (result.kind == 'f') imp = reinterpret_cast<void *>(&native_stub_float);
    else if (result.homogeneous) imp = reinterpret_cast<void *>(&native_stub_hfa);
    method->entry = {sel, imp, method->encoding.c_str(), false, method.get()};
    const char *risk = inferred && placeholder ? "risk:inferred-abi" : "modeled";
    if (result.kind == '^' || result.kind == '*') risk = "risk:pointer";
    for (std::size_t i = 3; i < method->signature.size(); ++i)
        if (method->signature[i].kind == '^') risk = "risk:out-param";
    if (inferred && !placeholder) risk = "risk:unknown-abi";
    std::fprintf(stderr, "stub: [%s] %c[%s %s] %s\n", risk, rw(c)->is_meta ? '+' : '-', rw(c)->name.c_str(), sel, method->encoding.c_str());
    auto *entry = &method->entry;
    have_dynamic_methods = true;
    rw(c)->stubs.emplace(sel, std::move(method));
    rw(c)->cache[sel] = *entry;
    return entry;
}

const method_entry *resolve_stub(Class c, SEL sel) {
    /* Caller holds g_lock. Strict mode preserves fail-fast diagnostics. */
    static const bool strict = std::getenv("MACHOMAN_STRICT_SELECTORS") != nullptr;
    if (strict) return nullptr;
    bool host = false, dynamic = false;
    const char *types = nullptr;
    std::string encoding, property;
    for (Class k = c; k; k = k->superclass) {
        auto *r = rw(k);
        if (!r || r->missing_super) return nullptr;
        host |= r->host; dynamic |= r->dynamic_stubs;
        if (auto it = r->stubs.find(sel); it != r->stubs.end()) return &it->second->entry;
        if (encoding.empty() && r->ro) property_signature(r->ro->baseProperties, sel, encoding, property);
        if (!types) for (auto *protocol : r->protocols) {
            if (!rw(c)->is_meta && encoding.empty()) property_signature(protocol->instanceProperties, sel, encoding, property);
            if (auto *found = protocol_signature(protocol, sel, rw(c)->is_meta)) { types = found; break; }
        }
    }
    if (!host) return nullptr;
    if (!encoding.empty()) return install_stub(c, sel, std::move(encoding), false, dynamic, std::move(property));
    if (types) return install_stub(c, sel, types, false, dynamic);
    const std::string_view name(sel);
    const auto colons = static_cast<std::size_t>(std::count(name.begin(), name.end(), ':'));
    /* Unknown ABI cannot promise a struct or output layout. Only a
     * synthesized class opts into conservative selector-based modeling. */
    encoding = "Q@:";
    if (dynamic && !colons && !name.starts_with("is") && !name.starts_with("has") &&
        !name.starts_with("can") && !name.starts_with("supports") && name != "count" && name != "length" && name != "hash" && name != "status") encoding = "@@:";
    else if (dynamic && stub_family(name, "init")) encoding = "@@:";
    bool setter = false; auto key = default_property(name, setter);
    if (dynamic && !colons) {
        std::string setter_name = "set" + key;
        if (setter_name.size() > 3) setter_name[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter_name[3])));
        setter_name += ':';
        if (auto it = rw(c)->stubs.find(sel_intern_copy(setter_name.c_str())); it != rw(c)->stubs.end() && it->second->setter) {
            auto text = std::string_view(it->second->encoding);
            stub_abi::parse(text); /* result */
            stub_abi::parse(text); /* self */
            stub_abi::parse(text); /* selector */
            encoding = std::string(text) + "@:";
        }
    }
    if (dynamic && setter) {
        /* Pair with an already typed getter. Otherwise preserve a word,
         * without interpreting an unknown value as a retainable object. */
        if (auto it = rw(c)->stubs.find(sel_intern_copy(key.c_str())); it != rw(c)->stubs.end()) {
            auto text = std::string_view(it->second->encoding);
            auto rest = text; stub_abi::parse(rest);
            encoding = "v@:" + std::string(text.substr(0, text.size() - rest.size()));
        } else encoding = "v@:Q";
    } else encoding.append(colons, 'Q');
    return install_stub(c, sel, std::move(encoding), true, dynamic);
}

std::uint64_t invoke_stub(mm_cpu_context &ctx, dynamic_method &method) {
    std::unique_lock lock(g_lock);
    id receiver = reinterpret_cast<id>(ctx.x[0]);
    const auto &result = method.signature[0];
    if (method.initializer) return reinterpret_cast<std::uintptr_t>(receiver);
    auto &values = T().stub_values;
    if (method.setter) {
        const auto &argument = method.signature[3];
        auto &value = values[receiver][method.property];
        auto &bytes = value.bytes;
        bytes.resize(argument.size);
        std::fill(bytes.begin(), bytes.end(), 0);
        if (argument.homogeneous) {
            const std::size_t width = argument.homogeneous == 'f' ? 4 : 8;
            for (std::size_t i = 0; i < argument.elements; ++i) std::memcpy(bytes.data() + i * width, &ctx.v[i][0], width);
        } else if (argument.size > 16) {
            if (ctx.x[2]) std::memcpy(bytes.data(), reinterpret_cast<void *>(ctx.x[2]), bytes.size());
        } else if (argument.size) std::memcpy(bytes.data(), ctx.x + 2, bytes.size());
        id object = nullptr;
        if (argument.kind == '@') { std::memcpy(&object, bytes.data(), sizeof object); do_retain(object); }
        id previous = value.object; value.object = object;
        do_release(previous);
        return 0;
    }
    dynamic_value *value = nullptr;
    if (method.property) if (auto it = values.find(receiver); it != values.end())
        if (auto stored = it->second.find(method.property); stored != it->second.end()) value = &stored->second;
    if (result.kind == '@' && method.placeholder) {
        if (value) return reinterpret_cast<std::uintptr_t>(value->object);
        Class target = nullptr;
        if (!result.object_class.empty()) {
            auto name = result.object_class.substr(0, result.object_class.find('<'));
            target = mm_objc_class_named(name.c_str());
        } else target = rw(receiver->isa)->is_meta ? reinterpret_cast<Class>(receiver) : receiver->isa;
        lock.unlock();
        id object = target ? reinterpret_cast<id>(mm_objc_send0(reinterpret_cast<id>(target), "new")) : nullptr;
        lock.lock();
        if (method.property) {
            auto &properties = values[receiver];
            auto [stored, inserted] = properties.try_emplace(method.property);
            if (!inserted) {
                id existing = stored->second.object;
                lock.unlock(); do_release(object);
                return reinterpret_cast<std::uintptr_t>(existing);
            }
            stored->second.object = object;
            return reinterpret_cast<std::uintptr_t>(object);
        }
        if (!stub_family(method.entry.sel, "new") && !stub_family(method.entry.sel, "copy") && !stub_family(method.entry.sel, "mutableCopy")) mm_objc_autorelease(object);
        return reinterpret_cast<std::uintptr_t>(object);
    }
    if (result.kind == '#') return reinterpret_cast<std::uintptr_t>(rw(receiver->isa)->is_meta ? reinterpret_cast<Class>(receiver) : receiver->isa);
    ctx.x[0] = 0;
    if (result.kind == '{' || result.kind == '[') ctx.x[1] = 0;
    if (result.homogeneous) {
        const auto width = result.homogeneous == 'f' ? 4u : 8u;
        for (std::size_t i = 0; i < result.elements; ++i) {
            ctx.v[i][0] = ctx.v[i][1] = 0;
            if (value && value->bytes.size() == result.size) std::memcpy(&ctx.v[i][0], value->bytes.data() + i * width, width);
        }
    } else if (result.size > 16) {
        /* AAPCS64 indirect result register. Never guess an x8 buffer size. */
        if (ctx.x[8]) {
            auto *out = reinterpret_cast<void *>(ctx.x[8]);
            if (value && value->bytes.size() == result.size) std::memcpy(out, value->bytes.data(), result.size);
            else std::memset(out, 0, result.size);
        }
    } else if (value && value->bytes.size() == result.size && result.size) {
        std::memcpy(ctx.x, value->bytes.data(), result.size);
    } else if (method.inferred) {
        /* Unknown returns may be FP, but an indirect result cannot be
         * touched without a known layout. Diagnostics retain that risk. */
        std::memset(ctx.v, 0, sizeof(ctx.v[0]) * 4);
    }
    return ctx.x[0];
}

} // namespace
extern "C" {
void mm_objc_enable_dynamic_stubs(mm_objc_class *cls) {
    std::lock_guard lock(g_lock);
    if (cls && rw(cls)) { rw(cls)->dynamic_stubs = true; rw(cls->isa)->dynamic_stubs = true; }
}
int mm_objc_add_stub_method(mm_objc_class *cls, const char *selector, const char *types) {
    if (!cls || !selector || !types) return 0;
    std::lock_guard lock(g_lock);
    SEL sel = sel_intern_copy(selector);
    if (lookup_in(cls, sel) || rw(cls)->stubs.contains(sel)) return 0;
    return install_stub(cls, sel, types, false, true) != nullptr;
}
}
namespace {
