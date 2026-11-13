#define GL_SILENCE_DEPRECATION
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#import <AppKit/AppKit.h>
#include <OpenGL/gl3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "OpenGL threads: %s\n", message); exit(1); }
}
static void *worker(void *unused) {
    (void)unused;
    @autoreleasepool {
        NSOpenGLPixelFormatAttribute attributes[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core, 0
        };
        NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
        NSOpenGLContext *context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
        check([context CGLContextObj] != NULL, "create context on fresh pthread");
        [context makeCurrentContext];
        const GLubyte *version = glGetString(GL_VERSION);
        check(version != NULL, "worker context is current for real GL calls");
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        check(major > 3 || (major == 3 && minor >= 2), "desktop OpenGL 3.2 available");
        printf("Worker OpenGL: %s\n", version);
        [NSOpenGLContext clearCurrentContext];
        [context release]; [format release];
    }
    return NULL;
}
int main(void) {
    if (!getenv("WAYLAND_DISPLAY") || getenv("MACHOMAN_APPKIT_HEADLESS")) return 77;
    @autoreleasepool {
        // Initialize the display and EGL client API on the main thread first.
        NSOpenGLPixelFormatAttribute attributes[] = {0};
        NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
        NSOpenGLContext *context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
        check([context CGLContextObj] != NULL, "initialize main-thread EGL display");
        pthread_t thread;
        check(pthread_create(&thread, NULL, worker, NULL) == 0, "start worker");
        check(pthread_join(thread, NULL) == 0, "join worker");
        [context release]; [format release];
    }
    puts("AppKit desktop OpenGL context creation on a fresh thread passed");
    return 0;
}
