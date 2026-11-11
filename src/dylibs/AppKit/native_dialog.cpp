#include "native_dialog.h"

#include <atomic>
#include <cstdlib>
#ifdef MM_APPKIT_GIO
#include <gio/gio.h>
#endif
#ifdef MM_APPKIT_GTK
#include <gtk/gtk.h>
#endif

namespace ak {
namespace {
class failed_dialog final : public native_dialog {
    std::string error_;
public:
    explicit failed_dialog(std::string error) : error_(std::move(error)) {}
    bool poll(dialog_result &out) override { out = {false, {}, error_}; return true; }
    void cancel() override {}
};
#ifdef MM_APPKIT_GIO
constexpr const char *portal_name = "org.freedesktop.portal.Desktop";
constexpr const char *request_interface = "org.freedesktop.portal.Request";
GMainContext *portal_context() { static auto *context = g_main_context_new(); return context; }
struct portal_state {
    GMainContext *context = g_main_context_ref(portal_context());
    GDBusConnection *bus = nullptr;
    std::string path;
    guint subscription = 0;
    bool done = false, cancelled = false;
    dialog_result result;
    ~portal_state() {
        if (bus) g_object_unref(bus);
        g_main_context_unref(context);
    }
    void close_request() {
        if (bus && !path.empty())
            g_dbus_connection_call(bus, portal_name, path.c_str(), request_interface, "Close",
                nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr, nullptr);
    }
};
using state_ptr = std::shared_ptr<portal_state>;
void response(GDBusConnection *, const gchar *, const gchar *path, const gchar *,
              const gchar *, GVariant *parameters, gpointer data) {
    auto &s = **static_cast<state_ptr *>(data);
    if (s.cancelled || s.done || s.path != path) return;
    guint32 code = 2;
    GVariant *values = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &code, &values);
    if (code == 0) {
        GVariant *uris = g_variant_lookup_value(values, "uris", G_VARIANT_TYPE_STRING_ARRAY);
        if (uris) {
            GVariantIter iter; g_variant_iter_init(&iter, uris);
            const gchar *uri = nullptr;
            while (g_variant_iter_next(&iter, "&s", &uri)) {
                GError *error = nullptr;
                gchar *filename = g_filename_from_uri(uri, nullptr, &error);
                if (filename) { s.result.paths.emplace_back(filename); g_free(filename); }
                else { s.result.error = error ? error->message : "Invalid file URI"; }
                g_clear_error(&error);
            }
            g_variant_unref(uris);
        }
        s.result.accepted = !s.result.paths.empty() && s.result.error.empty();
        if (!s.result.accepted && s.result.error.empty()) s.result.error = "Portal returned no files";
    } else if (code != 1) s.result.error = "File chooser portal failed";
    g_variant_unref(values);
    if (!s.result.accepted) s.result.paths.clear();
    s.done = true;
}
void method_reply(GObject *source, GAsyncResult *reply, gpointer data) {
    std::unique_ptr<state_ptr> holder(static_cast<state_ptr *>(data));
    auto &s = **holder;
    GError *error = nullptr;
    GVariant *value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), reply, &error);
    if (value) {
        const gchar *path = nullptr; g_variant_get(value, "(&o)", &path);
        s.path = path;
        if (s.cancelled) s.close_request();
        g_variant_unref(value);
    } else if (!s.cancelled && !s.done) {
        s.result.error = error ? error->message : "File chooser portal unavailable";
        s.done = true;
    }
    g_clear_error(&error);
}
class portal_dialog final : public native_dialog {
    state_ptr s_ = std::make_shared<portal_state>();
public:
    explicit portal_dialog(const file_dialog_options &o) {
        auto &s = *s_;
        g_main_context_push_thread_default(s.context);
        GError *error = nullptr;
        s.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!s.bus) {
            s.result.error = error ? error->message : "Session bus unavailable";
            s.done = true; g_clear_error(&error);
            g_main_context_pop_thread_default(s.context); return;
        }
        static std::atomic<unsigned> sequence{0};
        const std::string token = "machoman_" + std::to_string(++sequence);
        std::string sender = g_dbus_connection_get_unique_name(s.bus);
        if (!sender.empty() && sender.front() == ':') sender.erase(0, 1);
        for (char &c : sender) if (c == '.') c = '_';
        s.path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
        s.subscription = g_dbus_connection_signal_subscribe(s.bus, portal_name, request_interface,
            "Response", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, response, new state_ptr(s_),
            [](gpointer p) { delete static_cast<state_ptr *>(p); });
        GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(true));
        if (!o.accept_label.empty()) g_variant_builder_add(&options, "{sv}", "accept_label", g_variant_new_string(o.accept_label.c_str()));
        if (!o.folder.empty()) g_variant_builder_add(&options, "{sv}", "current_folder", g_variant_new_bytestring(o.folder.c_str()));
        if (o.save) {
            if (!o.name.empty()) g_variant_builder_add(&options, "{sv}", "current_name", g_variant_new_string(o.name.c_str()));
        } else {
            g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(o.multiple));
            g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(o.directories));
        }
        if (!o.filters.empty() && !o.directories) {
            GVariantBuilder filters; g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
            for (const auto &f : o.filters) {
                GVariantBuilder entries; g_variant_builder_init(&entries, G_VARIANT_TYPE("a(us)"));
                for (const auto &p : f.patterns) g_variant_builder_add(&entries, "(us)", 0u, p.c_str());
                for (const auto &m : f.mime_types) g_variant_builder_add(&entries, "(us)", 1u, m.c_str());
                g_variant_builder_add(&filters, "(s@a(us))", f.name.c_str(), g_variant_builder_end(&entries));
            }
            g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));
        }
        g_dbus_connection_call(s.bus, portal_name, "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.FileChooser", o.save ? "SaveFile" : "OpenFile",
            g_variant_new("(ss@a{sv})", o.parent.c_str(), o.title.c_str(), g_variant_builder_end(&options)),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, method_reply, new state_ptr(s_));
        g_main_context_pop_thread_default(s.context);
    }
    ~portal_dialog() override { cancel(); }
    bool poll(dialog_result &out) override {
        // Bound dispatch so a busy bus cannot starve AppKit's event loop.
        for (int i = 0; i < 32 && g_main_context_iteration(s_->context, false); ++i) {}
        if (!s_->done) return false;
        out = s_->result; return true;
    }
    void cancel() override {
        if (s_->cancelled) return;
        s_->cancelled = true;
        if (!s_->done) s_->close_request();
        // The pending method owns its state until method_reply. Keep the call
        // alive so its returned handle can also be closed after cancellation.
        if (s_->subscription) {
            g_dbus_connection_signal_unsubscribe(s_->bus, s_->subscription);
            s_->subscription = 0;
        }
        s_->result = {}; s_->done = true;
    }
};
#endif
#ifdef MM_APPKIT_GTK
class message_dialog final : public native_dialog {
    GtkWidget *window_ = nullptr;
    int response_ = -1;
public:
    message_dialog(const std::string &message, const std::string &detail,
                   const std::vector<std::string> &buttons, unsigned style) {
        window_ = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL,
            style == 2 ? GTK_MESSAGE_ERROR : style == 0 ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING,
            GTK_BUTTONS_NONE, "%s", message.c_str());
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(window_), "%s", detail.c_str());
        if (buttons.empty()) gtk_dialog_add_button(GTK_DIALOG(window_), "OK", 0);
        for (std::size_t i = 0; i < buttons.size(); ++i)
            gtk_dialog_add_button(GTK_DIALOG(window_), buttons[i].c_str(), static_cast<int>(i));
        gtk_dialog_set_default_response(GTK_DIALOG(window_), 0);
        g_signal_connect(window_, "response", G_CALLBACK(+[](GtkDialog *, gint code, gpointer p) {
            static_cast<message_dialog *>(p)->response_ = code < 0 ? GTK_RESPONSE_CANCEL : code;
        }), this);
        gtk_widget_show(window_);
    }
    ~message_dialog() override { gtk_widget_destroy(window_); }
    bool poll(dialog_result &out) override {
        for (int i = 0; i < 32 && g_main_context_iteration(nullptr, false); ++i) {}
        if (response_ == -1) return false;
        out.accepted = response_ >= 0;
        if (out.accepted) out.paths = {std::to_string(response_)};
        return true;
    }
    void cancel() override { response_ = GTK_RESPONSE_CANCEL; gtk_widget_hide(window_); }
};
#endif
}
void dispatch_native_dialogs() {
#ifdef MM_APPKIT_GIO
    for (int i = 0; i < 32 && g_main_context_iteration(portal_context(), false); ++i) {}
#endif
#ifdef MM_APPKIT_GTK
    for (int i = 0; i < 32 && g_main_context_iteration(nullptr, false); ++i) {}
#endif
}
bool open_external_uri(const std::string &uri) {
    if (std::getenv("MACHOMAN_APPKIT_HEADLESS")) return false;
#ifdef MM_APPKIT_GIO
    GError *error = nullptr;
    const bool opened = g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error);
    if (error) { g_warning("Cannot open URI: %s", error->message); g_clear_error(&error); }
    return opened;
#else
    (void)uri;
    return false;
#endif
}
std::unique_ptr<native_dialog> open_file_dialog(const file_dialog_options &o) {
    if (std::getenv("MACHOMAN_APPKIT_HEADLESS")) return std::make_unique<failed_dialog>("File chooser disabled in headless mode");
#ifdef MM_APPKIT_GIO
    return std::make_unique<portal_dialog>(o);
#else
    (void)o;
    return std::make_unique<failed_dialog>("File chooser requires GIO and xdg-desktop-portal");
#endif
}
std::unique_ptr<native_dialog> open_message_dialog(const std::string &message,
    const std::string &detail, const std::vector<std::string> &buttons, unsigned style) {
#ifdef MM_APPKIT_GTK
    if (!std::getenv("MACHOMAN_APPKIT_HEADLESS") && gtk_init_check(nullptr, nullptr))
        return std::make_unique<message_dialog>(message, detail, buttons, style);
#else
    (void)message; (void)detail; (void)buttons; (void)style;
#endif
    return std::make_unique<failed_dialog>("Message dialogs require GTK and a display");
}
}
