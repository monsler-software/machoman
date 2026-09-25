/* The stub framework: a host library that defines nothing and answers
 * every bind with a synthesised symbol. The venv links one copy of it
 * under each framework machoman does not implement yet. */
#include "synthesise.h"

extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}
