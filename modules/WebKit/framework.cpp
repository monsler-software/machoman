#include "synthesise.h"
#include "machoman/objc_host.h"
extern mm_objc_class webview_class __asm__("_OBJC_CLASS_$_WKWebView");
extern "C" __attribute__((visibility("default"))) mm_objc_class *machoman_webkit_view_class() {
    return &webview_class; // keep the AppKit dependency even with --as-needed
}
extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}
