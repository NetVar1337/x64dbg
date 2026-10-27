// Loads a library after startup, then stops so the test can look for it.
#include <dlfcn.h>

int main(void)
{
    void* handle = dlopen("/usr/lib/libz.1.dylib", RTLD_NOW);
    __builtin_debugtrap();
    return handle ? 42 : 1;
}
