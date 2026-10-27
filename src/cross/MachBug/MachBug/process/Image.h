#pragma once

#include <MachBug/thread/Arch.h>
#include <mach/mach.h>

namespace MachBug
{
    // Finds the main executable by scanning the address space for its Mach-O header, so it
    // works before dyld has run. Returns 0 if there is none or it has no LC_MAIN.
    ptr FindEntryPoint(task_t task);
}
