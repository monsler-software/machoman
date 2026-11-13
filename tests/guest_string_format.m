#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(NSString *value, const char *expected) {
    if (!value || strcmp([value UTF8String], expected)) {
        fprintf(stderr, "string formatting: expected '%s', got '%s'\n",
                expected, value ? [value UTF8String] : "(nil)");
        exit(1);
    }
}
static NSString *via_va_list(NSString *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    return s;
}
static CFStringRef cf_via_va_list(CFStringRef fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    CFStringRef s = CFStringCreateWithFormatAndArguments(NULL, NULL, fmt, ap);
    va_end(ap);
    return s;
}
int main(void) {
    @autoreleasepool {
        const char *expected = "object 7 2.50 3 4 5 6";
        check([NSString stringWithFormat:@"%@ %d %.2f %d %d %d %d", @"object", 7, 2.5, 3, 4, 5, 6], expected);
        check(via_va_list(@"%@ %d %.2f %d %d %d %d", @"object", 7, 2.5, 3, 4, 5, 6), expected);
        check([[NSString alloc] initWithFormat:@"%@ %d %.1f" locale:nil, @"locale", 11, 3.5], "locale 11 3.5");
        check((NSString *)CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %d %.2f %d %d %d %d"),
                                                  CFSTR("object"), 7, 2.5, 3, 4, 5, 6), expected);
        check((NSString *)cf_via_va_list(CFSTR("%@ %d %.2f %d %d %d %d"),
                                         CFSTR("object"), 7, 2.5, 3, 4, 5, 6), expected);
        NSMutableString *s = [NSMutableString stringWithString:@"start"];
        [s appendFormat:@" %@ %d %.1f", @"objc", 9, 4.5];
        CFStringAppendFormat((CFMutableStringRef)s, NULL, CFSTR(" %@ %d %.1f"), CFSTR("cf"), 13, 6.5);
        check(s, "start objc 9 4.5 cf 13 6.5");
        check([NSString stringWithFormat:@"%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f",
                                         1., 2., 3., 4., 5., 6., 7., 8., 9., 10.], "1 2 3 4 5 6 7 8 9 10");
        check([NSString stringWithFormat:@"%d %.2Lf %d", 17, (long double)1.25, 19], "17 1.25 19");
        char text[80];
        snprintf(text, sizeof text, "%d %.2Lf %d", 17, (long double)1.25, 19);
        if (strcmp(text, "17 1.25 19")) abort();
        NSLog(@"NSLog ABI: %s %@ %d %.1f %d %d %d %d", "C string", @"object", 7, 2.5, 3, 4, 5, 6);
    }
    puts("Darwin NSString and CFString formatting passed");
    return 0;
}
