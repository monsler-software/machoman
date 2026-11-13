#import <Foundation/Foundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "JSON: %s\n", message); exit(1); }
}
static id parse(NSString *text, NSJSONReadingOptions options, NSError **error) {
    return [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding] options:options error:error];
}
static NSString *json_write(id object, NSJSONWritingOptions options, NSError **error) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:options error:error];
    return data ? [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease] : nil;
}
int main(void) {
    @autoreleasepool {
        NSError *error = nil;
        NSDictionary *launch = parse(@"{\"placeId\":1838614938,\"joinAttemptId\":\"test\",\"launchData\":\"hello\",\"nested\":[true,false,null,{\"name\":\"\\u043f\\u0440\\u0438\\u0432\\u0435\\u0442 \\ud83d\\ude80\"}]}", 0, &error);
        check(launch && !error && [launch isKindOfClass:NSDictionary.class], "launch parameters return dictionary without error");
        check([[launch objectForKey:@"placeId"] longLongValue] == 1838614938, "placeId survives JSON parsing");
        check([[[launch objectForKey:@"joinAttemptId"] description] isEqualToString:@"test"], "string parameter");
        NSUInteger count = 0;
        for (NSString *key in launch) { check([launch objectForKey:key] != nil, "message-bus dictionary enumeration"); ++count; }
        check(count == 4, "all launch keys enumerate");
        NSArray *nested = [launch objectForKey:@"nested"];
        check([[nested objectAtIndex:0] boolValue] && ![[nested objectAtIndex:1] boolValue], "JSON booleans");
        check([nested objectAtIndex:2] == NSNull.null, "null is NSNull");
        check([[[nested objectAtIndex:3] objectForKey:@"name"] isEqualToString:@"привет 🚀"], "Unicode escapes and surrogate pair");
        NSArray *numbers = parse(@"[-9223372036854775808,18446744073709551615,9007199254740993,1.25e2]", 0, &error);
        check([[numbers objectAtIndex:0] longLongValue] == (-9223372036854775807LL - 1), "signed lower boundary");
        check([[numbers objectAtIndex:1] unsignedLongLongValue] == 18446744073709551615ULL, "unsigned upper boundary");
        check([[numbers objectAtIndex:2] longLongValue] == 9007199254740993LL, "integer precision beyond double");
        check([[numbers objectAtIndex:3] doubleValue] == 125.0, "decimal exponent");
        NSString *encoded = json_write(launch, NSJSONWritingSortedKeys, &error);
        check(encoded && !error && [encoded rangeOfString:@"\n"].location == NSNotFound, "compact writer");
        check([parse(encoded, 0, &error) isEqual:launch], "Cocoa objects survive write/read round trip");
        check([json_write(numbers, 0, &error) isEqualToString:@"[-9223372036854775808,18446744073709551615,9007199254740993,125]"], "lossless integer output");
        check([json_write(@[@(18446744073709551615ULL), @(-1)], 0, &error) isEqualToString:@"[18446744073709551615,-1]"], "signed and unsigned integer literals");
        check(strcmp([@1 objCType], "i") == 0, "integer literal encoding");
        NSDictionary *sortInput = @{ @"z":@1, @"a":@YES, @"path":@"a/b" };
        NSString *sorted = json_write(sortInput, NSJSONWritingSortedKeys, &error);
        check([sorted isEqualToString:@"{\"a\":true,\"path\":\"a\\/b\",\"z\":1}"], "sorted keys, boolean identity and slash escaping");
        check([json_write(@[@"a/b"], NSJSONWritingWithoutEscapingSlashes, &error) isEqualToString:@"[\"a/b\"]"], "unescaped slash option");
        check([json_write(@{ @"a":@[@1] }, NSJSONWritingPrettyPrinted, &error) rangeOfString:@"\n"].location != NSNotFound, "pretty output");
        check(![NSJSONSerialization isValidJSONObject:@1] && [NSJSONSerialization isValidJSONObject:launch], "validity requires container root");
        check([parse(@"true", NSJSONReadingFragmentsAllowed, &error) boolValue], "reading scalar fragments");
        check([json_write(@"hello", NSJSONWritingFragmentsAllowed, &error) isEqualToString:@"\"hello\""], "writing scalar fragments");
        NSMutableDictionary *mutable = parse(@"{\"text\":\"a\",\"array\":[\"b\"]}", NSJSONReadingMutableContainers | NSJSONReadingMutableLeaves, &error);
        [[mutable objectForKey:@"text"] appendString:@"c"];
        [[mutable objectForKey:@"array"] addObject:@1];
        [mutable setObject:@2 forKey:@"new"];
        check([[mutable objectForKey:@"text"] isEqualToString:@"ac"] && [[mutable objectForKey:@"array"] count] == 2 && [[mutable objectForKey:@"new"] intValue] == 2, "mutable leaves and nested containers");
        NSString *document = @"{\"name\":\"привет 🚀\"}";
        const NSStringEncoding encodings[] = { NSUTF8StringEncoding, NSUTF16LittleEndianStringEncoding, NSUTF16BigEndianStringEncoding, NSUTF32LittleEndianStringEncoding, NSUTF32BigEndianStringEncoding };
        for (unsigned i = 0; i < sizeof(encodings)/sizeof(encodings[0]); ++i) {
            NSData *data = [document dataUsingEncoding:encodings[i]];
            NSDictionary *decoded = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
            check(decoded && !error && [[decoded objectForKey:@"name"] isEqualToString:@"привет 🚀"], "Unicode input encodings");
        }
        const unsigned char bom[] = { 0xef,0xbb,0xbf,'[',']' };
        check([[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bom length:sizeof(bom)] options:0 error:&error] count] == 0, "UTF-8 BOM");
        for (NSString *bad in @[@"", @"{", @"[1,]", @"{\"a\":1,}", @"[01]", @"[1e999]", @"[\"\\ud800\"]", @"[true] trailing", @"42"]) {
            error = nil;
            check(!parse(bad, 0, &error) && error, "invalid JSON reports NSError");
            check([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == 3840 && error.localizedDescription.length > 0, "JSON error domain, code and description");
            check(!parse(bad, 0, NULL), "optional error output");
        }
        const unsigned char invalid[] = { '[','"',0xc0,0xaf,'"',']' };
        check(![NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:invalid length:sizeof(invalid)] options:0 error:&error] && error, "invalid UTF-8");
        check(parse(@"{}", 0, &error) && !error, "success clears stale error");
        check([[parse(@"{\"key\":1,\"key\":2}", 0, &error) objectForKey:@"key"] intValue] == 2 && !error, "duplicate keys keep their last value");
        for (id bad in @[@{ @1:@"value" }, @[@(NAN)], @[@(INFINITY)], @[NSDate.date]]) {
            check(![NSJSONSerialization isValidJSONObject:bad], "unsupported values are invalid");
            check(!json_write(bad, 0, &error) && error.code == 3840, "writer reports invalid values");
        }
        NSMutableArray *cycle = [NSMutableArray array];
        id cycleElement = cycle;
        [cycle addObject:cycleElement];
        check(![NSJSONSerialization isValidJSONObject:cycle] && !json_write(cycle, 0, &error), "cyclic graph rejected");
        [cycle removeAllObjects];
    }
    puts("Cocoa JSON parsing, writing and errors passed");
    return 0;
}
