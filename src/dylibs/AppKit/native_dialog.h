#ifndef MACHOMAN_NATIVE_DIALOG_H
#define MACHOMAN_NATIVE_DIALOG_H

#include <memory>
#include <string>
#include <vector>

namespace ak {
struct file_filter { std::string name; std::vector<std::string> patterns, mime_types; };
struct file_dialog_options {
    std::string title, accept_label, folder, name, parent;
    std::vector<file_filter> filters;
    bool save = false, directories = false, multiple = false;
};
struct dialog_result {
    bool accepted = false;
    std::vector<std::string> paths;
    std::string error;
};
/* Requests run on the caller's thread; poll never waits for user input. */
class native_dialog {
public:
    virtual ~native_dialog() = default;
    virtual bool poll(dialog_result &result) = 0;
    virtual void cancel() = 0;
};
void dispatch_native_dialogs();
bool open_external_uri(const std::string &uri);
std::unique_ptr<native_dialog> open_file_dialog(const file_dialog_options &options);
std::unique_ptr<native_dialog> open_message_dialog(const std::string &message,
    const std::string &detail, const std::vector<std::string> &buttons, unsigned style);
}
#endif
