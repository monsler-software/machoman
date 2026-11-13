#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "file URL: %s\n", message); exit(1); }
}
int main(void) {
    @autoreleasepool {
        NSURL *base = [[NSURL alloc] initFileURLWithPath:@"/tmp/url base" isDirectory:YES];
        NSURL *relative = [[NSURL alloc] initFileURLWithPath:@"sub/../file %.txt" relativeToURL:base];
        check(relative.baseURL == base, "base URL identity");
        [base release];
        check([relative.path isEqualToString:@"/tmp/url base/file %.txt"], "resolved directory base");
        check([relative.relativePath isEqualToString:@"sub/../file %.txt"], "relative path retained");
        check([relative.relativeString isEqualToString:@"sub/../file%20%25.txt"], "relative string encoding");
        check([relative.absoluteString isEqualToString:@"file:///tmp/url%20base/file%20%25.txt"], "absolute string encoding");
        check(strcmp(relative.fileSystemRepresentation, "/tmp/url base/file %.txt") == 0, "filesystem representation");
        check(relative.absoluteURL.baseURL == nil && [relative.absoluteURL.path isEqualToString:relative.path], "absolute URL removes base");
        NSURL *fileBase = [NSURL fileURLWithPath:@"/tmp/base/file.txt" isDirectory:NO];
        NSURL *child = [NSURL fileURLWithPath:@"child" relativeToURL:fileBase];
        check([child.path isEqualToString:@"/tmp/base/child"], "file base uses parent directory");
        NSURL *absolute = [NSURL fileURLWithPath:@"/var/data" relativeToURL:fileBase];
        check(absolute.baseURL == nil && [absolute.path isEqualToString:@"/var/data"], "absolute path overrides base");
        NSURL *directory = [NSURL fileURLWithPath:@"folder" isDirectory:YES relativeToURL:relative.baseURL];
        check(directory.hasDirectoryPath && [directory.absoluteString hasSuffix:@"/folder/"], "explicit directory");
        check([directory.relativeString isEqualToString:@"folder/"], "relative directory terminator");
        NSURL *fs = [NSURL fileURLWithFileSystemRepresentation:"data %/файл" isDirectory:NO relativeToURL:fileBase];
        check(strcmp(fs.fileSystemRepresentation, "/tmp/base/data %/файл") == 0, "UTF-8 filesystem constructor");
        NSURL *fsInit = [[NSURL alloc] initFileURLWithFileSystemRepresentation:"data %/файл" isDirectory:NO relativeToURL:fileBase];
        check([fsInit.path isEqualToString:fs.path], "filesystem initializer");
        NSURL *dirInit = [[NSURL alloc] initFileURLWithPath:@"folder" isDirectory:YES relativeToURL:fileBase];
        check([dirInit.path isEqualToString:@"/tmp/base/folder"] && dirInit.hasDirectoryPath, "directory initializer");
        char cwd[4096];
        check(getcwd(cwd, sizeof cwd) != NULL, "getcwd");
        NSURL *current = [NSURL fileURLWithPath:@"plain" relativeToURL:nil];
        NSString *expected = [[NSString stringWithUTF8String:cwd] stringByAppendingPathComponent:@"plain"];
        check([current.path isEqualToString:expected] && current.baseURL == nil, "nil base uses current directory");
        check([[NSURL fileURLWithPath:@"/tmp"].absoluteString hasSuffix:@"/tmp/"], "existing directory detection");
        [relative release]; [fsInit release]; [dirInit release];
    }
    puts("Darwin file URL constructors and base resolution passed");
    return 0;
}
