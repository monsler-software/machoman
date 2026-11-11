#include "../src/dylibs/AppKit/native_dialog.h"
#include <gio/gio.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

void require(bool ok, const char *message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
const char xml[] = R"(<node><interface name="org.freedesktop.portal.FileChooser">
<method name="OpenFile"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="SaveFile"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface><interface name="org.freedesktop.portal.Request"><method name="Close"/></interface></node>)";
struct fixture {
    GDBusConnection *bus;
    GDBusNodeInfo *info;
    int calls = 0, closes = 0;
    unsigned code = 0;
    bool fail = false;
    GVariant *options = nullptr;
    std::string method, parent, title;
};
void call(GDBusConnection *, const gchar *sender, const gchar *, const gchar *, const gchar *method,
          GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
    auto &f = *static_cast<fixture *>(data);
    if (std::string_view(method) == "Close") { ++f.closes; g_dbus_method_invocation_return_value(invocation, nullptr); return; }
    ++f.calls; f.method = method;
    if (f.fail) { g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.Failed", "Test failure"); return; }
    const char *parent = nullptr, *title = nullptr;
    if (f.options) g_variant_unref(f.options);
    g_variant_get(parameters, "(&s&s@a{sv})", &parent, &title, &f.options);
    f.parent = parent; f.title = title;
    const char *token = nullptr; require(g_variant_lookup(f.options, "handle_token", "&s", &token), "handle token");
    std::string name = sender + 1; for (auto &c : name) if (c == '.') c = '_';
    std::string path = "/org/freedesktop/portal/desktop/request/" + name + "/" + token;
    static const GDBusInterfaceVTable vtable{call, nullptr, nullptr, {}};
    g_dbus_connection_register_object(f.bus, path.c_str(), f.info->interfaces[1], &vtable, &f, nullptr, nullptr);
    GVariantBuilder results; g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    if (f.code == 0) {
        const char *uris[] = {"file:///tmp/test%20folder/%D1%84%D0%B0%D0%B9%D0%BB.txt", "file:///tmp/second.txt", nullptr};
        g_variant_builder_add(&results, "{sv}", "uris", g_variant_new_strv(uris, -1));
    }
    if (f.code != 99)
        g_dbus_connection_emit_signal(f.bus, sender, path.c_str(), "org.freedesktop.portal.Request", "Response",
            g_variant_new("(u@a{sv})", f.code, g_variant_builder_end(&results)), nullptr);
    else g_variant_builder_clear(&results);
    // Exercise the documented response-before-method-reply race.
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path.c_str()));
}
ak::dialog_result wait(ak::native_dialog &dialog) {
    ak::dialog_result result;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
        while (g_main_context_iteration(nullptr, false)) {}
        if (dialog.poll(result)) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(false, "dialog timed out"); return {};
}
int main() {
    unsetenv("MACHOMAN_APPKIT_HEADLESS");
    GTestDBus *server = g_test_dbus_new(G_TEST_DBUS_NONE); g_test_dbus_up(server);
    GError *error = nullptr;
    fixture f{g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error), g_dbus_node_info_new_for_xml(xml, &error)};
    require(f.bus && f.info && !error, "private bus setup");
    auto *reply = g_dbus_connection_call_sync(f.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0u), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    require(reply && !error, "portal name"); g_variant_unref(reply);
    static const GDBusInterfaceVTable vtable{call, nullptr, nullptr, {}};
    auto registration = g_dbus_connection_register_object(f.bus, "/org/freedesktop/portal/desktop", f.info->interfaces[0], &vtable, &f, nullptr, &error);
    require(registration && !error, "portal registration");
    ak::file_dialog_options o; o.title = "Select files"; o.multiple = true; o.parent = "wayland:test";
    o.folder = "/tmp/test folder"; o.filters = {{"Text", {"*.txt"}, {"text/plain"}}};
    auto dialog = ak::open_file_dialog(o); auto result = wait(*dialog);
    require(result.accepted && result.paths.size() == 2 && result.paths[0] == "/tmp/test folder/файл.txt", "multiple decoded file URLs");
    require(f.method == "OpenFile" && f.parent == o.parent && f.title == o.title, "open request metadata");
    gboolean multiple = false; require(g_variant_lookup(f.options, "multiple", "b", &multiple) && multiple, "multiple option");
    auto *folder = g_variant_lookup_value(f.options, "current_folder", G_VARIANT_TYPE_BYTESTRING);
    require(folder && std::string(g_variant_get_bytestring(folder)) == o.folder, "NUL terminated folder bytes"); g_variant_unref(folder);
    auto *filters = g_variant_lookup_value(f.options, "filters", G_VARIANT_TYPE("a(sa(us))"));
    require(filters && g_variant_n_children(filters) == 1, "typed filters"); g_variant_unref(filters);
    o.save = true; o.name = "output.txt"; dialog = ak::open_file_dialog(o); result = wait(*dialog);
    const char *name = nullptr;
    require(result.accepted && f.method == "SaveFile" && g_variant_lookup(f.options, "current_name", "&s", &name) && name == o.name, "save request");
    f.code = 1; o.save = false; o.directories = true; dialog = ak::open_file_dialog(o); result = wait(*dialog);
    gboolean directory = false; require(g_variant_lookup(f.options, "directory", "b", &directory) && directory, "directory selection");
    require(!result.accepted && result.paths.empty() && result.error.empty(), "user cancellation");
    f.fail = true; dialog = ak::open_file_dialog(o); result = wait(*dialog);
    require(!result.accepted && result.error.find("Test failure") != std::string::npos, "method error");
    f.fail = false; f.code = 99; dialog = ak::open_file_dialog(o);
    const int before = f.calls;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (f.calls == before && std::chrono::steady_clock::now() < until) { g_main_context_iteration(nullptr, false); dialog->poll(result); }
    require(f.calls > before, "pending dialog submitted"); dialog->cancel();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!f.closes && std::chrono::steady_clock::now() < until) { g_main_context_iteration(nullptr, false); ak::dispatch_native_dialogs(); }
    require(f.closes && !wait(*dialog).accepted, "programmatic cancellation closes portal request");
    dialog.reset();
    g_dbus_connection_unregister_object(f.bus, registration);
    g_variant_unref(f.options); g_dbus_node_info_unref(f.info);
    g_dbus_connection_close_sync(f.bus, nullptr, nullptr); g_object_unref(f.bus);
    g_test_dbus_down(server); g_object_unref(server);
    std::puts("portal open, save, folders, filters, Unicode, response race and cancellation passed");
}
