#pragma once

#include <MachBug/thread/Arch.h>
#include <mach/mach.h>
#include <string>
#include <utility>
#include <vector>

namespace MachBug
{
    struct Module
    {
        ptr base = 0; // its Mach-O header
        std::string path;
        // Segment ranges (start, size), without __PAGEZERO and the __LINKEDIT that every
        // image in the shared cache shares.
        std::vector<std::pair<ptr, ptr>> ranges;

        [[nodiscard]] bool Contains(ptr address) const;
    };

    // Finds the main executable by scanning the address space for its Mach-O header, so it
    // works before dyld has run. Returns 0 if there is none or it has no LC_MAIN.
    ptr FindEntryPoint(task_t task);

    // The images dyld has loaded plus dyld itself, sorted by base; before dyld has run, the
    // executable and dyld. Modules in known are reused rather than read again.
    std::vector<Module> ReadModules(task_t task, pid_t pid, const std::vector<Module> & known);
}
