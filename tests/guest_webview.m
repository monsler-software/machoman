#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void check(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "webview: %s\n", message); exit(1); }
}
static unsigned cookieCallbacks, messageCallbacks, navigations, evaluations;
@interface WebProbe : NSObject <WKNavigationDelegate, WKScriptMessageHandler>
@end
@implementation WebProbe
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation {
    ++navigations;
    [view evaluateJavaScript:@"({answer: 6 * 7, text: 'Привет'})" completionHandler:^(id result, NSError *error) {
        check(error == nil && [result[@"answer"] intValue] == 42 && [result[@"text"] isEqual:@"Привет"],
            "JavaScript completion carries structured values across the guest ABI");
        ++evaluations;
    }];
}
- (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    fprintf(stderr, "webview navigation: %s\n", error.localizedDescription.UTF8String); exit(1);
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    check([message.name isEqual:@"probe"] && [message.body[@"answer"] intValue] == 42 && message.webView != nil,
        "window.webkit.messageHandlers posts a typed Cocoa message");
    ++messageCallbacks;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool {
        BOOL browser = argc > 1;
        char pagePath[] = "/tmp/machoman-webview-XXXXXX.html";
        (void)NSApplication.sharedApplication;
        NSString *address = @"https://www.roblox.com/login?returnUrl=%2Fhome&mode=app#section";
        NSURL *url = [NSURL URLWithString:address];
        check([url.absoluteString isEqual:address] && [url.host isEqual:@"www.roblox.com"] &&
            [url.path isEqual:@"/login"] && [url.query isEqual:@"returnUrl=%2Fhome&mode=app"] &&
            [url.fragment isEqual:@"section"], "HTTP URL preserves query delimiters and existing percent escapes");
        NSURLRequest *request = [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:12.5];
        check(request.URL == url && [request.HTTPMethod isEqual:@"GET"] && request.timeoutInterval == 12.5 &&
            request.cachePolicy == NSURLRequestReloadIgnoringLocalCacheData && request.HTTPShouldHandleCookies,
            "URL requests preserve their address, HTTP defaults and floating timeout on both guest ABIs");
        NSMutableURLRequest *mutableRequest = [request mutableCopy];
        mutableRequest.HTTPMethod = @"POST";
        mutableRequest.HTTPBody = [@"payload" dataUsingEncoding:NSUTF8StringEncoding];
        [mutableRequest setValue:@"first" forHTTPHeaderField:@"X-Test"];
        [mutableRequest addValue:@"second" forHTTPHeaderField:@"x-test"];
        NSURLRequest *snapshot = [mutableRequest copy];
        [mutableRequest setValue:nil forHTTPHeaderField:@"X-TEST"];
        check([request.HTTPMethod isEqual:@"GET"] && [snapshot.HTTPMethod isEqual:@"POST"] &&
            snapshot.HTTPBody.length == 7 && [[snapshot valueForHTTPHeaderField:@"X-TEST"] isEqual:@"first,second"] &&
            mutableRequest.allHTTPHeaderFields.count == 0 && snapshot.allHTTPHeaderFields.count == 1,
            "request copies keep independent methods, bodies and case-insensitive headers");
        [snapshot release]; [mutableRequest release];
        check([[[NSData dataWithBytes:"\000\377\001\002" length:4] base64EncodedStringWithOptions:0] isEqual:@"AP8BAg=="] &&
            [[[NSData dataWithBytes:"fo" length:2] base64EncodedStringWithOptions:0] isEqual:@"Zm8="] &&
            [[[NSData dataWithBytes:"foo" length:3] base64EncodedStringWithOptions:0] isEqual:@"Zm9v"],
            "HTTP request body encoding preserves binary bytes and padding");
        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
        check(configuration.websiteDataStore == WKWebsiteDataStore.defaultDataStore &&
            configuration.userContentController != (id)configuration &&
            configuration.websiteDataStore.httpCookieStore != (id)configuration,
            "configuration owns distinct correctly typed child objects");
        WKWebsiteDataStore *temporary = WKWebsiteDataStore.nonPersistentDataStore;
        check(!temporary.persistent && configuration.websiteDataStore.persistent, "persistent and transient data stores");
        NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:@{NSHTTPCookieName:@"webview-test",
            NSHTTPCookieValue:@"value", NSHTTPCookieDomain:@"localhost", NSHTTPCookiePath:@"/"}];
        [temporary.httpCookieStore setCookie:cookie completionHandler:^{ ++cookieCallbacks; }];
        [temporary.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
            check(cookies.count == 1 && [cookies[0].value isEqual:@"value"], "cookie store asynchronous round trip");
            ++cookieCallbacks;
        }];
        configuration.websiteDataStore = temporary;
        WebProbe *probe = [WebProbe new];
        [configuration.userContentController addScriptMessageHandler:probe name:@"probe"];
        WKUserScript *script = [[WKUserScript alloc] initWithSource:@"window.webkit.messageHandlers.probe.postMessage({answer:42});"
            injectionTime:WKUserScriptInjectionTimeAtDocumentEnd forMainFrameOnly:YES];
        [configuration.userContentController addUserScript:script]; [script release];
        WKWebView *view = [[WKWebView alloc] initWithFrame:NSMakeRect(5, 10, 400, 300) configuration:configuration];
        view.navigationDelegate = probe;
        check([view isKindOfClass:NSView.class] && view.frame.size.width == 400 && view.configuration != configuration &&
            view.configuration.websiteDataStore == temporary, "NSView payload, Intel CGRect arguments and configuration copy");
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 500, 400)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        [window.contentView addSubview:view positioned:NSWindowAbove relativeTo:nil];
        check(view.window == window && view.superview == window.contentView, "WebView joins the real view tree without heap corruption");
        if (browser) {
            NSString *html = @"<html><title>WebView test</title><body>local page</body></html>";
            if (argc > 2) [view loadHTMLString:html baseURL:[NSURL URLWithString:@"http://localhost/"]];
            else {
                int fd = mkstemps(pagePath, 5);
                check(fd >= 0, "create local browser fixture");
                FILE *file = fdopen(fd, "w");
                check(file != NULL, "open local browser fixture");
                fputs(html.UTF8String, file); fclose(file);
                NSURL *local = [NSURL fileURLWithPath:[NSString stringWithUTF8String:pagePath]];
                [view loadRequest:[NSURLRequest requestWithURL:local]];
            }
        }
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15];
        while (cookieCallbacks < 2 || (browser && (!messageCallbacks || !evaluations))) {
            if (deadline.timeIntervalSinceNow <= 0)
                fprintf(stderr, "webview callbacks: cookies=%u messages=%u navigation=%u evaluation=%u\n",
                    cookieCallbacks, messageCallbacks, navigations, evaluations);
            check(deadline.timeIntervalSinceNow > 0, "callbacks reach the main Cocoa queue");
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .05, true);
        }
        if (browser) check(navigations > 0 && [view.title isEqual:@"WebView test"], "navigation delegates and page title");
        if (browser && argc <= 2) unlink(pagePath);
        view.navigationDelegate = nil;
        [view.configuration.userContentController removeScriptMessageHandlerForName:@"probe"];
        [view removeFromSuperview]; [view release]; [window release]; [configuration release]; [probe release];
    }
    puts("WKWebView hierarchy, configuration, cookies and guest callbacks passed");
    return 0;
}
