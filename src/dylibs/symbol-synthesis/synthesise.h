/* Synthesising a framework symbol from its name, for host libraries that
 * do not define every symbol a guest binds to. Linked into the stub
 * framework and into the real frameworks, whose `machoman_resolve_symbol`
 * forwards here. See synthesise.cpp for what each kind of name produces
 * and MM_SYMBOL_LAZY in runtime.h for how functions are told from data. */
#ifndef MACHOMAN_SYNTHESISE_H
#define MACHOMAN_SYNTHESISE_H

void *mm_synthesise_symbol(const char *name, unsigned flags);

#endif
