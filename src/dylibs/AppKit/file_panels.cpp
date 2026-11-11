#include "appkit.h"
#include "native_dialog.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>

MM_FW_CLASS(NSSavePanel);
MM_FW_CLASS(NSOpenPanel);

namespace ak {
namespace {
struct panel_object : window_object {
    std::string message, prompt, name, directory;
    id types = nullptr, content_types = nullptr, accessory = nullptr, urls = nullptr;
    id handler = nullptr, modal_delegate = nullptr;
    SEL did_end = nullptr;
    void *context = nullptr;
    id sheet_parent = nullptr;
    bool files = true, directories = false, multiple = false, create_directories = true;
    bool other_types = false, hidden_files = false, extension_hidden = false;
    bool can_select_hidden_extension = false, packages = false, resolves_aliases = true;
    bool active = false, finished = false;
    std::uint64_t response = 0;
    std::unique_ptr<native_dialog> request;
    ~panel_object() { release(types); release(content_types); release(accessory); release(urls); release(handler); }
};
std::vector<id> active_panels;
panel_object *panel(id self) { return as<panel_object>(self); }
void assign(id &slot, id value) { id old = slot; slot = retain(value); release(old); }
bool is_open(id self) { return mm_objc_is_kind_of(self, cls("NSOpenPanel")); }
std::uint64_t panel_factory(id class_, SEL) { return ret(autorelease(obj(send(obj(send(class_, "alloc")), "init")))); }

file_dialog_options options(id self) {
    auto *p = panel(self);
    file_dialog_options o;
    o.save = !is_open(self); o.multiple = p->multiple;
    o.directories = p->directories && !p->files;
    o.title = !p->message.empty() ? p->message : !p->title.empty() ? p->title : o.save ? "Save" : "Open";
    o.accept_label = p->prompt; o.folder = p->directory; o.name = p->name;
    if (p->sheet_parent) o.parent = platform::dialog_parent(as<window_object>(p->sheet_parent)->platform);
    file_filter filter; filter.name = "Allowed files";
    for (std::uint64_t i = 0; i < send(p->types, "count"); ++i) {
        auto ext = to_utf8(obj(send(p->types, "objectAtIndex:", i)));
        if (!ext.empty()) filter.patterns.push_back("*." + ext);
    }
    for (std::uint64_t i = 0; i < send(p->content_types, "count"); ++i) {
        id type = obj(send(p->content_types, "objectAtIndex:", i));
        if (responds(type, "preferredFilenameExtension")) {
            auto ext = to_utf8(obj(send(type, "preferredFilenameExtension")));
            if (!ext.empty()) filter.patterns.push_back("*." + ext);
        }
        if (responds(type, "preferredMIMEType")) {
            auto mime = to_utf8(obj(send(type, "preferredMIMEType")));
            if (!mime.empty()) filter.mime_types.push_back(mime);
        }
    }
    if (!filter.patterns.empty() || !filter.mime_types.empty()) o.filters.push_back(std::move(filter));
    // Supply the extension in the suggested name, before the native chooser
    // performs its overwrite check. Never change the accepted path afterwards.
    if (o.save && !o.name.empty() && std::filesystem::path(o.name).extension().empty() &&
        !o.filters.empty() && !o.filters[0].patterns.empty() && !p->other_types)
        o.name += o.filters[0].patterns[0].substr(1);
    if (p->other_types) o.filters.push_back({"All files", {"*"}, {}});
    return o;
}
void start(id self, id parent, id handler) {
    auto *p = panel(self);
    if (p->active) return;
    p->sheet_parent = parent; release(p->handler);
    p->handler = handler ? obj(send(handler, "copy")) : nullptr;
    assign(p->urls, nullptr); p->response = 0; p->finished = false; p->active = true;
    p->request = open_file_dialog(options(self));
    active_panels.push_back(retain(self));
}
void finish(id self, dialog_result result) {
    auto *p = panel(self);
    p->response = result.accepted ? 1 : 0;
    if (result.accepted) {
        id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
        for (auto path : result.paths) {
            id url = obj(send(reinterpret_cast<id>(cls("NSURL")), "fileURLWithPath:", ret(string(path))));
            if (responds(p->delegate, "panel:validateURL:error:")) {
                id error = nullptr;
                if (!send(p->delegate, "panel:validateURL:error:", ret(self), ret(url), ret(&error))) {
                    p->request = open_file_dialog(options(self)); return;
                }
            }
            send(array, "addObject:", ret(url));
            p->directory = std::filesystem::path(path).parent_path().string();
            p->name = std::filesystem::path(path).filename().string();
        }
        assign(p->urls, array);
    }
    if (!result.error.empty()) std::fprintf(stderr, "AppKit: file dialog: %s\n", result.error.c_str());
    p->request.reset(); p->active = false; p->finished = true; p->sheet_parent = nullptr;
    auto handler = p->handler; p->handler = nullptr;
    auto delegate = p->modal_delegate; auto selector = p->did_end; auto context = p->context;
    p->modal_delegate = nullptr; p->did_end = nullptr;
    const auto response = p->response;
    if (handler) { mm_objc_block_invoke(handler, 1, &response); release(handler); }
    if (delegate && selector) mm_objc_send(delegate, selector, 3,
        std::array<std::uint64_t, 3>{ret(self), response, ret(context)}.data());
}
std::uint64_t run_modal(id self, SEL) {
    if (panel(self)->active) return 0;
    retain(self); start(self, nullptr, nullptr);
    while (!panel(self)->finished) application_dialog_tick(nullptr);
    auto response = panel(self)->response; release(self); return response;
}
std::uint64_t begin(id self, SEL, id handler) { start(self, nullptr, handler); return 0; }
std::uint64_t begin_sheet(id self, SEL, id window, id handler) { start(self, window, handler); return 0; }
std::uint64_t cancel(id self, SEL, id) { if (panel(self)->request) panel(self)->request->cancel(); platform::wake(); return 0; }
std::uint64_t visible(id self, SEL) { return panel(self)->active; }
std::uint64_t url(id self, SEL) { return send(panel(self)->urls, "count") ? send(panel(self)->urls, "objectAtIndex:", 0) : 0; }
std::uint64_t urls(id self, SEL) { return ret(panel(self)->urls ? panel(self)->urls : obj(send(reinterpret_cast<id>(cls("NSArray")), "array"))); }
std::uint64_t filename(id self, SEL) { return send(obj(url(self, nullptr)), "path"); }
std::uint64_t filenames(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (std::uint64_t i = 0; i < send(panel(self)->urls, "count"); ++i)
        send(array, "addObject:", send(obj(send(panel(self)->urls, "objectAtIndex:", i)), "path"));
    return ret(array);
}
std::uint64_t directory_url(id self, SEL) { return panel(self)->directory.empty() ? 0 : send(reinterpret_cast<id>(cls("NSURL")), "fileURLWithPath:isDirectory:", ret(string(panel(self)->directory)), 1); }
std::uint64_t set_directory_url(id self, SEL, id value) { panel(self)->directory = to_utf8(obj(send(value, "path"))); return 0; }
std::uint64_t legacy_modal(id self, SEL, id dir, id file) {
    panel(self)->directory = to_utf8(dir); panel(self)->name = to_utf8(file); return run_modal(self, nullptr);
}
std::uint64_t legacy_open(id self, SEL, id dir, id file, id types) { assign(panel(self)->types, types); return legacy_modal(self, nullptr, dir, file); }
std::uint64_t legacy_sheet(id self, SEL, id dir, id file, id window, id delegate, SEL selector, void *context) {
    panel(self)->directory = to_utf8(dir); panel(self)->name = to_utf8(file);
    panel(self)->modal_delegate = delegate; panel(self)->did_end = selector; panel(self)->context = context;
    start(self, window, nullptr); return 0;
}
template <std::string panel_object::*member> std::uint64_t get_string(id self, SEL) { return ret(string(panel(self)->*member)); }
template <std::string panel_object::*member> std::uint64_t set_string(id self, SEL, id value) { panel(self)->*member = to_utf8(value); return 0; }
template <id panel_object::*member> std::uint64_t get_object(id self, SEL) { return ret(panel(self)->*member); }
template <id panel_object::*member> std::uint64_t set_object(id self, SEL, id value) {
    if constexpr (member == &panel_object::types || member == &panel_object::content_types) {
        auto previous = panel(self)->*member; panel(self)->*member = value ? obj(send(value, "copy")) : nullptr; release(previous);
    } else assign(panel(self)->*member, value);
    return 0;
}
template <bool panel_object::*member> std::uint64_t get_bool(id self, SEL) { return panel(self)->*member; }
template <bool panel_object::*member> std::uint64_t set_bool(id self, SEL, std::uint64_t value) { panel(self)->*member = value != 0; return 0; }
}
void poll_file_panels() {
    const auto snapshot = active_panels;
    for (id self : snapshot) retain(self);
    for (id self : snapshot) {
        if (std::find(active_panels.begin(), active_panels.end(), self) == active_panels.end()) { release(self); continue; }
        dialog_result result;
        if (!panel(self)->request || !panel(self)->request->poll(result)) { release(self); continue; }
        // Remove before invoking guest code, which may open another panel.
        auto it = std::find(active_panels.begin(), active_panels.end(), self);
        if (it != active_panels.end()) active_panels.erase(it);
        finish(self, std::move(result));
        if (panel(self)->active) active_panels.push_back(self);
        else release(self);
        release(self);
    }
}
bool file_panel_blocks_window(id window) {
    for (id self : active_panels) if (!panel(self)->sheet_parent || panel(self)->sheet_parent == window) return true;
    return false;
}
void register_file_panels() {
    Class save = mm_objc_define_class("NSSavePanel", cls("NSPanel"), sizeof(panel_object), MM_FW_CLASS_REF(NSSavePanel), MM_FW_META_REF(NSSavePanel));
    Class open = mm_objc_define_class("NSOpenPanel", save, sizeof(panel_object), MM_FW_CLASS_REF(NSOpenPanel), MM_FW_META_REF(NSOpenPanel));
    register_lifecycle<panel_object>(save); register_lifecycle<panel_object>(open);
    MM_CLASS_METHODS(save, {"savePanel", MM_IMP(panel_factory), "@16@0:8"});
    MM_CLASS_METHODS(open, {"openPanel", MM_IMP(panel_factory), "@16@0:8"});
    MM_METHODS(save,
        {"runModal", MM_IMP(run_modal), "q16@0:8"},
        {"beginWithCompletionHandler:", MM_IMP(begin), "v24@0:8@?16"},
        {"beginSheetModalForWindow:completionHandler:", MM_IMP(begin_sheet), "v32@0:8@16@?24"},
        {"cancel:", MM_IMP(cancel), "v24@0:8@16"},
        {"isVisible", MM_IMP(visible), "B16@0:8"},
        {"URL", MM_IMP(url), "@16@0:8"}, {"filename", MM_IMP(filename), "@16@0:8"},
        {"directoryURL", MM_IMP(directory_url), "@16@0:8"}, {"setDirectoryURL:", MM_IMP(set_directory_url), "v24@0:8@16"},
        {"runModalForDirectory:file:", MM_IMP(legacy_modal), "q32@0:8@16@24"},
        {"beginSheetForDirectory:file:modalForWindow:modalDelegate:didEndSelector:contextInfo:", MM_IMP(legacy_sheet), "v64@0:8@16@24@32@40:48^v56"});
    MM_METHODS(open, {"URLs", MM_IMP(urls), "@16@0:8"}, {"filenames", MM_IMP(filenames), "@16@0:8"},
        {"runModalForDirectory:file:types:", MM_IMP(legacy_open), "q40@0:8@16@24@32"});
#define STRING_PROPERTY(getter, setter, member) \
    mm_objc_add_method(save, getter, MM_IMP(get_string<&panel_object::member>), "@16@0:8"); \
    mm_objc_add_method(save, setter, MM_IMP(set_string<&panel_object::member>), "v24@0:8@16")
    STRING_PROPERTY("message", "setMessage:", message);
    STRING_PROPERTY("prompt", "setPrompt:", prompt);
    STRING_PROPERTY("nameFieldStringValue", "setNameFieldStringValue:", name);
    STRING_PROPERTY("directory", "setDirectory:", directory);
#undef STRING_PROPERTY
#define OBJECT_PROPERTY(getter, setter, member) \
    mm_objc_add_method(save, getter, MM_IMP(get_object<&panel_object::member>), "@16@0:8"); \
    mm_objc_add_method(save, setter, MM_IMP(set_object<&panel_object::member>), "v24@0:8@16")
    OBJECT_PROPERTY("allowedFileTypes", "setAllowedFileTypes:", types);
    OBJECT_PROPERTY("allowedContentTypes", "setAllowedContentTypes:", content_types);
    OBJECT_PROPERTY("accessoryView", "setAccessoryView:", accessory);
#undef OBJECT_PROPERTY
#define BOOL_PROPERTY(getter, setter, member) \
    mm_objc_add_method(save, getter, MM_IMP(get_bool<&panel_object::member>), "B16@0:8"); \
    mm_objc_add_method(save, setter, MM_IMP(set_bool<&panel_object::member>), "v24@0:8B16")
    BOOL_PROPERTY("canChooseFiles", "setCanChooseFiles:", files);
    BOOL_PROPERTY("canChooseDirectories", "setCanChooseDirectories:", directories);
    BOOL_PROPERTY("allowsMultipleSelection", "setAllowsMultipleSelection:", multiple);
    BOOL_PROPERTY("canCreateDirectories", "setCanCreateDirectories:", create_directories);
    BOOL_PROPERTY("allowsOtherFileTypes", "setAllowsOtherFileTypes:", other_types);
    BOOL_PROPERTY("showsHiddenFiles", "setShowsHiddenFiles:", hidden_files);
    BOOL_PROPERTY("isExtensionHidden", "setExtensionHidden:", extension_hidden);
    BOOL_PROPERTY("canSelectHiddenExtension", "setCanSelectHiddenExtension:", can_select_hidden_extension);
    BOOL_PROPERTY("treatsFilePackagesAsDirectories", "setTreatsFilePackagesAsDirectories:", packages);
    BOOL_PROPERTY("resolvesAliases", "setResolvesAliases:", resolves_aliases);
#undef BOOL_PROPERTY
}
}
