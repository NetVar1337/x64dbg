#include <MachBug/process/Image.h>
#include <MachBug/process/Memory.h>
#include <mach-o/loader.h>
#include <vector>

namespace MachBug
{
    ptr FindEntryPoint(const task_t task)
    {
        for(const auto & region : ReadMemoryMap(task))
        {
            mach_header_64 header = {};
            if(!(region.protection & VM_PROT_READ) || !ReadMemory(task, region.base, &header, sizeof(header)))
                continue;
            if(header.magic != MH_MAGIC_64 || header.filetype != MH_EXECUTE)
                continue;

            std::vector<uint8_t> commands(header.sizeofcmds);
            if(!ReadMemory(task, region.base + sizeof(header), commands.data(), commands.size()))
                return 0;

            ptr offset = 0;
            for(uint32_t i = 0; i < header.ncmds && offset + sizeof(load_command) <= commands.size(); ++i)
            {
                const auto* command = reinterpret_cast<const load_command*>(commands.data() + offset);
                if(command->cmdsize < sizeof(load_command) || offset + command->cmdsize > commands.size())
                    return 0;
                // entryoff is relative to __TEXT, which starts at the header.
                if(command->cmd == LC_MAIN && command->cmdsize >= sizeof(entry_point_command))
                    return region.base + reinterpret_cast<const entry_point_command*>(command)->entryoff;
                offset += command->cmdsize;
            }
            return 0;
        }
        return 0;
    }
}
