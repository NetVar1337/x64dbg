#pragma once

#include <MachBug/thread/Arch.h>
#include <mach/mach.h>
#include <vector>

namespace MachBug
{
    struct MemoryRegion
    {
        ptr base = 0;
        ptr size = 0;
        vm_prot_t protection = VM_PROT_NONE;
    };

    bool ReadMemory(task_t task, ptr address, void* dest, size_t size);

    // Code pages are mapped without write access, so those get a private writable copy.
    bool WriteMemory(task_t task, ptr address, const void* src, size_t size);

    // Leaf regions only: submaps such as the shared cache are walked into.
    std::vector<MemoryRegion> ReadMemoryMap(task_t task);

    bool FindMemoryRegion(task_t task, ptr address, MemoryRegion & region);
}
