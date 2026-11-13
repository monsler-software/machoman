#ifndef MACHOMAN_DISPATCH_HOST_H
#define MACHOMAN_DISPATCH_HOST_H

// Host frameworks use a scope for callbacks whose code/data must be retired
// before dlclose. Stop cancels queued callbacks (including on the main queue)
// and waits for running callbacks. It must not be called from its own callback.
// invoke owns context on execution; cancel owns it if execution is cancelled.
extern "C" {
void *mm_dispatch_scope_create();
void mm_dispatch_scope_async(void *scope, void *queue, void *context,
                             void (*invoke)(void *), void (*cancel)(void *));
void mm_dispatch_scope_stop(void *scope);
void mm_dispatch_scope_destroy(void *scope);
}

#endif
