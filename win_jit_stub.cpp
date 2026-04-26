#ifdef _WIN32

#include <cstddef>

// These Jitter functions are called unconditionally by the Max SDK, but aren't
// needed by our external. Override them with no-ops so we avoid linking with
// Jitter.
extern "C" {
int jit_class_addattr(void*, void*) { return 0; }

int max_jit_class_addattr(void*, void*) { return 0; }
}

#endif
