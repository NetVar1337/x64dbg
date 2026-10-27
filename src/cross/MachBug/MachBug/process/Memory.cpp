#include <MachBug/process/Memory.h>
#include <mach/mach_vm.h>
#include <algorithm>

namespace MachBug
{
    namespace
    {
        // The region at or above address, descending into submaps.
        bool nextLeafRegion(const task_t task, const ptr address, MemoryRegion & region)
        {
            mach_vm_address_t base = address;
            natural_t depth = 0;
            for(;;)
            {
                mach_vm_size_t size = 0;
                vm_region_submap_short_info_data_64_t info = {};
                mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
                if(mach_vm_region_recurse(task, &base, &size, &depth,
                                          reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS)
                    return false;
                if(info.is_submap)
                {
                    ++depth;
                    base = std::max<mach_vm_address_t>(base, address);
                    continue;
                }
                region = {base, size, info.protection};
                return true;
            }
        }

        bool writeWithinPage(const task_t task, const ptr address, const uint8_t* bytes, const size_t size)
        {
            const auto data = reinterpret_cast<vm_offset_t>(bytes);
            const auto count = static_cast<mach_msg_type_number_t>(size);
            if(mach_vm_write(task, address, data, count) == KERN_SUCCESS)
                return true;

            MemoryRegion region;
            if(!FindMemoryRegion(task, address, region))
                return false;
            if(mach_vm_protect(task, address, size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
                return false;
            const bool written = mach_vm_write(task, address, data, count) == KERN_SUCCESS;
            mach_vm_protect(task, address, size, false, region.protection);
            return written;
        }
    }

    bool ReadMemory(const task_t task, const ptr address, void* dest, const size_t size)
    {
        mach_vm_size_t read = 0;
        return mach_vm_read_overwrite(task, address, size, reinterpret_cast<mach_vm_address_t>(dest), &read) == KERN_SUCCESS &&
               read == size;
    }

    bool WriteMemory(const task_t task, ptr address, const void* src, size_t size)
    {
        const ptr start = address;
        const size_t total = size;
        const auto* bytes = static_cast<const uint8_t*>(src);
        while(size > 0)
        {
            // A write may span pages with different protections.
            const ptr pageEnd = (address & ~static_cast<ptr>(vm_page_size - 1)) + vm_page_size;
            const size_t chunk = std::min<size_t>(size, pageEnd - address);
            if(!writeWithinPage(task, address, bytes, chunk))
                return false;
            address += chunk;
            bytes += chunk;
            size -= chunk;
        }
        // Other cores may still run the old instructions from their caches.
        vm_machine_attribute_val_t flush = MATTR_VAL_CACHE_FLUSH;
        mach_vm_machine_attribute(task, start, total, MATTR_CACHE, &flush);
        return true;
    }

    std::vector<MemoryRegion> ReadMemoryMap(const task_t task)
    {
        std::vector<MemoryRegion> regions;
        MemoryRegion region;
        for(ptr address = 0; nextLeafRegion(task, address, region); address = region.base + region.size)
            regions.push_back(region);
        return regions;
    }

    bool FindMemoryRegion(const task_t task, const ptr address, MemoryRegion & region)
    {
        return nextLeafRegion(task, address, region) && region.base <= address;
    }
}
