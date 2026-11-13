#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>

static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "HTTP cookies: %s\n", message); exit(1); }
}
static NSArray *parse(NSString *header, NSURL *url) {
    return [NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie": header} forURL:url];
}
static NSHTTPCookie *cookie(NSString *name, NSString *value, NSString *domain, NSString *path) {
    return [NSHTTPCookie cookieWithProperties:@{NSHTTPCookieName:name, NSHTTPCookieValue:value,
        NSHTTPCookieDomain:domain, NSHTTPCookiePath:path}];
}
int main(void) {
    @autoreleasepool {
        NSURL *url = [NSURL URLWithString:@"https://www.roblox.com/auth/login"];
        NSHTTPCookieStorage *storage = [[NSHTTPCookieStorage alloc] init];
        NSArray *cookies = parse(@"session=synthetic-test-value; Domain=.roblox.com; Path=/; Secure; HttpOnly; Max-Age=3600, RBXEventTrackerV2=123; Path=/; Expires=Wed, 09 Jun 2038 10:18:14 GMT", url);
        check(cookies.count == 2, "folded header and Expires comma");
        NSHTTPCookie *session = [cookies objectAtIndex:0];
        check([session.name isEqualToString:@"session"] && [session.value isEqualToString:@"synthetic-test-value"], "name/value");
        check(session.isSecure && session.isHTTPOnly && !session.isSessionOnly, "attributes");
        check(session.expiresDate.timeIntervalSince1970 > NSDate.date.timeIntervalSince1970, "Max-Age expiry");
        [storage setCookies:cookies forURL:url mainDocumentURL:url];
        NSArray *api = [storage cookiesForURL:[NSURL URLWithString:@"https://users.roblox.com/v1/users/authenticated"]];
        check(api.count == 1, "domain cookies cross subdomains; host-only cookies do not");
        NSString *header = [[NSHTTPCookie requestHeaderFieldsWithCookies:api] objectForKey:@"Cookie"];
        check([header isEqualToString:@"session=synthetic-test-value"], "request Cookie header");
        check([storage cookiesForURL:[NSURL URLWithString:@"http://users.roblox.com/"]].count == 0, "Secure exclusion");
        check([storage cookiesForURL:[NSURL URLWithString:@"https://evilroblox.com/"]].count == 0, "domain boundary");
        check(parse(@"bad=1; Domain=example.com", url).count == 0, "reject unrelated response domain");
        NSHTTPCookie *scoped = cookie(@"scope", @"one", @"www.roblox.com", @"/auth");
        [storage setCookie:scoped];
        check([storage cookiesForURL:url].count == 3, "path match");
        check([storage cookiesForURL:[NSURL URLWithString:@"https://www.roblox.com/authenticate"]].count == 2, "path boundary");
        check([[[[storage cookiesForURL:url] objectAtIndex:0] name] isEqualToString:@"scope"], "longer paths first");
        [storage setCookie:cookie(@"scope", @"two", @"www.roblox.com", @"/auth")];
        check(storage.cookies.count == 3, "replace by name/domain/path");
        [storage deleteCookie:scoped];
        check(storage.cookies.count == 2, "delete by identity tuple");
        [storage setCookies:parse(@"session=gone; Domain=.roblox.com; Path=/; Max-Age=0; Expires=Wed, 09 Jun 2038 10:18:14 GMT", url) forURL:url mainDocumentURL:url];
        check(storage.cookies.count == 1, "Max-Age overrides Expires; expired cookie deletes");
        NSArray *defaults = parse(@"empty=", url);
        check(defaults.count == 1 && [[[defaults objectAtIndex:0] path] isEqualToString:@"/auth"], "default response path and empty value");
        check([[defaults objectAtIndex:0] isSessionOnly], "session-only cookie");
        check(parse(@"old=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT", url).count == 1, "epoch expiry parses");
        [storage setCookies:parse(@"old=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT", url) forURL:url mainDocumentURL:url];
        check(storage.cookies.count == 1, "epoch expiry excluded");
        [storage setCookies:parse(@"old=1; Max-Age=-1", url) forURL:url mainDocumentURL:url];
        check(storage.cookies.count == 1, "past expiry excluded");
        [storage setCookieAcceptPolicy:NSHTTPCookieAcceptPolicyNever];
        [storage setCookie:cookie(@"blocked", @"1", @"www.roblox.com", @"/")];
        check(storage.cookies.count == 1 && storage.cookieAcceptPolicy == NSHTTPCookieAcceptPolicyNever, "Never policy");
        [storage setCookieAcceptPolicy:NSHTTPCookieAcceptPolicyOnlyFromMainDocumentDomain];
        [storage setCookies:defaults forURL:url mainDocumentURL:[NSURL URLWithString:@"https://example.com/"]];
        check(storage.cookies.count == 1, "third-party rejection");
        [storage setCookies:defaults forURL:url mainDocumentURL:url];
        check(storage.cookies.count == 2, "first-party acceptance");
        NSMutableDictionary *properties = [NSMutableDictionary dictionaryWithDictionary:session.properties];
        NSHTTPCookie *copy = [NSHTTPCookie cookieWithProperties:properties];
        [properties setObject:@"mutated" forKey:NSHTTPCookieValue];
        check([copy.value isEqualToString:@"synthetic-test-value"], "properties snapshot");
        check([NSHTTPCookie cookieWithProperties:@{NSHTTPCookieName:@"invalid"}] == nil, "invalid properties");
        check([NSHTTPCookieStorage sharedHTTPCookieStorage] == [NSHTTPCookieStorage sharedHTTPCookieStorage], "shared storage identity");
        [storage release];
    }
    puts("Cocoa HTTP cookie parsing, request headers and storage passed");
    return 0;
}
