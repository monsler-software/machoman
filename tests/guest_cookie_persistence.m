#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr,"cookie persistence: %s\n",message); exit(1); }
}
int main(int argc, char **argv) {
    @autoreleasepool {
        check(argc == 2,"test phase");
        NSHTTPCookieStorage *store = [NSHTTPCookieStorage sharedHTTPCookieStorage];
        NSURL *url = [NSURL URLWithString:@"https://www.example.test/"];
        if (!strcmp(argv[1],"write")) {
            NSArray *cookies = [NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie":
                @"login=synthetic-one; Domain=.example.test; Path=/; Secure; HttpOnly; Max-Age=3600, temporary=synthetic; Path=/, expired=gone; Path=/; Max-Age=0"} forURL:url];
            [store setCookies:cookies forURL:url mainDocumentURL:url];
            check(store.cookies.count == 2,"persistent and session cookies visible in current process");
            NSMutableDictionary *properties = [NSMutableDictionary dictionaryWithDictionary:[[store.cookies objectAtIndex:0] properties]];
            properties[NSHTTPCookieValue] = @"synthetic-two";
            [store setCookie:[NSHTTPCookie cookieWithProperties:properties]];
        } else if (!strcmp(argv[1],"read")) {
            check(store.cookies.count == 1,"only unexpired persistent cookie restored");
            NSHTTPCookie *cookie = [store.cookies objectAtIndex:0];
            check([cookie.name isEqualToString:@"login"] && [cookie.value isEqualToString:@"synthetic-two"],"replacement survived restart");
            check(cookie.isSecure && cookie.isHTTPOnly && !cookie.isSessionOnly,"attributes survived restart");
            check([cookie.domain isEqualToString:@".example.test"] && [cookie.path isEqualToString:@"/"],"scope survived restart");
            check(cookie.expiresDate.timeIntervalSince1970 > NSDate.date.timeIntervalSince1970,"expiry survived restart");
            check([store cookiesForURL:[NSURL URLWithString:@"https://api.example.test/"]].count == 1,"domain scope after restart");
            check([store cookiesForURL:[NSURL URLWithString:@"http://api.example.test/"]].count == 0,"Secure exclusion after restart");
            check(cookie.properties[NSHTTPCookieMaximumAge] == nil,"expiry stored as absolute date");
        } else if (!strcmp(argv[1],"delete")) {
            for (NSHTTPCookie *cookie in store.cookies) [store deleteCookie:cookie];
        } else if (!strcmp(argv[1],"empty")) {
            check(store.cookies.count == 0,"deletion survived restart");
        } else check(NO,"known test phase");
    }
    return 0;
}
