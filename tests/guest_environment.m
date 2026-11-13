#import <Foundation/Foundation.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef environ
extern char **environ;
extern int mm_test_host_environment(void);
static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "environment failed: %s\n", what); exit(1); }
}
static int equals(const char *name, const char *value) {
    const char *actual=getenv(name); return actual && strcmp(actual, value)==0;
}
static void reap(pid_t child) {
    int status=0; require(waitpid(child, &status, 0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0, "child status");
}
int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1], "--empty")) {
        require(!strcmp(argv[0], "sh"), "preserve explicit argv0");
        require(*_NSGetEnviron() && !(*_NSGetEnviron())[0], "exact empty environment");
        return 0;
    }
    if (argc>1 && !strcmp(argv[1], "--inherited")) {
        require(equals("INHERITED", "live") && !getenv("MM_VENV_VALUE"), "inherit actual environment without JSON reinjection");
        return 0;
    }
    if (argc>1 && !strcmp(argv[1], "--explicit")) {
        require(equals("ONLY", "explicit") && !getenv("MM_VENV_VALUE"), "explicit envp without defaults");
        require((*_NSGetEnviron())[0] && !(*_NSGetEnviron())[1], "no launcher variables leaked");
        return 0;
    }
    require(argc==2, "child path supplied");
    require(equals("MM_VENV_VALUE", "from-json") && equals("MM_CLI_VALUE", "cli") && !getenv("MM_VENV_MASK"), "JSON defaults and CLI precedence");
    if (_NSGetEnviron()!=&environ || *_NSGetEnviron()!=environ)
        fprintf(stderr, "slots getter=%p exported=%p values getter=%p exported=%p\n", (void *)_NSGetEnviron(), (void *)&environ, (void *)*_NSGetEnviron(), (void *)environ);
    require(_NSGetEnviron()==&environ && *_NSGetEnviron()==environ, "shared exported environment slot");
    require(setenv("MM_HOST_CONTROL", "guest", 1)==0 && mm_test_host_environment(), "guest does not mutate host libc env");
    require(setenv("DYNAMIC", "new", 1)==0, "setenv");
    require([[[[NSProcessInfo processInfo] environment] objectForKey:@"DYNAMIC"] isEqualToString:@"new"], "Foundation observes live environment");
    require(setenv("HOME", "/Users/guest", 1)==0 && [NSHomeDirectory() isEqualToString:@"/Users/guest"], "Foundation HOME");
    char buffer[]="BORROW=first";
    require(putenv(buffer)==0, "putenv"); memcpy(buffer+7, "other", 5);
    require(equals("BORROW", "other"), "putenv caller storage");
    char **saved=environ;
    char direct[]="DIRECT=yes"; char *replacement[]={direct, NULL}; environ=replacement;
    require(equals("DIRECT", "yes") && !getenv("DYNAMIC"), "direct environ replacement");
    require(setenv("APPENDED", "value", 1)==0 && equals("DIRECT", "yes"), "setenv after replacement");
    environ=saved;
    require(unsetenv("MM_VENV_VALUE")==0 && setenv("INHERITED", "live", 1)==0, "prepare child environment");
    char *empty[]={NULL}; pid_t pid;
    char *empty_args[]={"sh", "--empty", NULL};
    require(posix_spawn(&pid, argv[1], NULL, NULL, empty_args, empty)==0, "spawn empty envp"); reap(pid);
    char only[]="ONLY=explicit"; char *explicit_env[]={only, NULL};
    char *explicit_args[]={"child", "--explicit", NULL};
    require(posix_spawn(&pid, argv[1], NULL, NULL, explicit_args, explicit_env)==0, "spawn explicit envp"); reap(pid);
    pid=fork(); require(pid>=0, "fork");
    if (pid==0) {
        char *args[]={"child", "--inherited", NULL}; execv(argv[1], args); _exit(127);
    }
    reap(pid);
    require(equals("INHERITED", "live") && !getenv("MM_VENV_VALUE"), "parent state survives child exec");
    puts("SDK live environment, Foundation, putenv, inheritance, empty envp and argv0 passed");
    return 0;
}
