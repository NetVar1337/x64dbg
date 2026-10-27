#include <MachBug/process/Image.h>
#include <MachBug/process/Memory.h>
#include <libproc.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <algorithm>
#include <climits>
#include <cstring>

namespace MachBug
{
    namespace
    {
        // Calls fn(command) for each load command until it returns false; false if they cannot be read.
        template<typename Fn>
        bool forEachCommand(const task_t task, const ptr base, mach_header_64 & header, Fn fn)
        {
            if(!ReadMemory(task, base, &header, sizeof(header)) || header.magic != MH_MAGIC_64)
                return false;
            std::vector<uint8_t> commands(header.sizeofcmds);
            if(!ReadMemory(task, base + sizeof(header), commands.data(), commands.size()))
                return false;
            size_t offset = 0;
            for(uint32_t i = 0; i < header.ncmds; ++i)
            {
                if(offset + sizeof(load_command) > commands.size())
                    return false;
                const auto & command = *reinterpret_cast<const load_command*>(commands.data() + offset);
                if(command.cmdsize < sizeof(load_command) || offset + command.cmdsize > commands.size())
                    return false;
                if(!fn(command))
                    break;
                offset += command.cmdsize;
            }
            return true;
        }

        bool isSegment(const segment_command_64 & segment, const char* name)
        {
            return strncmp(segment.segname, name, sizeof(segment.segname)) == 0;
        }

        std::string readString(const task_t task, const ptr address)
        {
            std::string text;
            char chunk[256];
            while(text.size() < PATH_MAX)
            {
                const ptr at = address + text.size();
                // The string may end right before an unmapped page.
                const size_t size = std::min<size_t>(sizeof(chunk), vm_page_size - (at & (vm_page_size - 1)));
                if(!ReadMemory(task, at, chunk, size))
                    break;
                const size_t length = strnlen(chunk, size);
                text.append(chunk, length);
                if(length < size)
                    break;
            }
            return text;
        }

        bool readModule(const task_t task, const pid_t pid, const ptr base, std::string path, Module & module)
        {
            mach_header_64 header = {};
            std::vector<segment_command_64> segments;
            std::string dylinker;
            const bool read = forEachCommand(task, base, header, [&](const load_command & command)
            {
                if(command.cmd == LC_SEGMENT_64 && command.cmdsize >= sizeof(segment_command_64))
                    segments.push_back(reinterpret_cast<const segment_command_64 &>(command));
                if(command.cmd == LC_ID_DYLINKER && command.cmdsize > sizeof(dylinker_command))
                {
                    const auto & id = reinterpret_cast<const dylinker_command &>(command);
                    if(id.name.offset < command.cmdsize)
                        dylinker.assign(reinterpret_cast<const char*>(&command) + id.name.offset,
                                        strnlen(reinterpret_cast<const char*>(&command) + id.name.offset, command.cmdsize - id.name.offset));
                }
                return true;
            });
            const auto text = std::find_if(segments.begin(), segments.end(), [](const segment_command_64 & segment)
            {
                return isSegment(segment, SEG_TEXT);
            });
            if(!read || text == segments.end())
                return false;

            if(path.empty() && header.filetype == MH_DYLINKER)
                path = dylinker;
            if(path.empty() && header.filetype == MH_EXECUTE)
            {
                char buffer[PROC_PIDPATHINFO_MAXSIZE] = {};
                if(proc_pidpath(pid, buffer, sizeof(buffer)) > 0)
                    path = buffer;
            }

            const ptr slide = base - text->vmaddr;
            module = {base, std::move(path), {}};
            for(const auto & segment : segments)
            {
                if(segment.vmsize != 0 && !isSegment(segment, SEG_PAGEZERO) && !isSegment(segment, SEG_LINKEDIT))
                    module.ranges.emplace_back(segment.vmaddr + slide, segment.vmsize);
            }
            return true;
        }
    }

    bool Module::Contains(const ptr address) const
    {
        return std::any_of(ranges.begin(), ranges.end(), [address](const std::pair<ptr, ptr> & range)
        {
            return address >= range.first && address - range.first < range.second;
        });
    }

    ptr FindEntryPoint(const task_t task)
    {
        for(const auto & region : ReadMemoryMap(task))
        {
            mach_header_64 header = {};
            if(!(region.protection & VM_PROT_READ) || !ReadMemory(task, region.base, &header, sizeof(header)) ||
                    header.magic != MH_MAGIC_64 || header.filetype != MH_EXECUTE)
                continue;

            ptr entry = 0;
            forEachCommand(task, region.base, header, [&](const load_command & command)
            {
                // entryoff is relative to __TEXT, which starts at the header.
                if(command.cmd == LC_MAIN && command.cmdsize >= sizeof(entry_point_command))
                    entry = region.base + reinterpret_cast<const entry_point_command &>(command).entryoff;
                return entry == 0;
            });
            return entry;
        }
        return 0;
    }

    std::vector<Module> ReadModules(const task_t task, const pid_t pid, const std::vector<Module> & known)
    {
        // Image bases with the address of their path, or 0 when it has to be found another way.
        std::vector<std::pair<ptr, ptr>> images;
        task_dyld_info_data_t dyld = {};
        mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
        dyld_all_image_infos infos = {};
        if(task_info(task, TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyld), &count) == KERN_SUCCESS &&
                dyld.all_image_info_addr != 0 &&
                ReadMemory(task, dyld.all_image_info_addr, &infos, std::min<size_t>(sizeof(infos), dyld.all_image_info_size)) &&
                infos.infoArray && infos.infoArrayCount != 0)
        {
            std::vector<dyld_image_info> list(infos.infoArrayCount);
            if(ReadMemory(task, reinterpret_cast<ptr>(infos.infoArray), list.data(), list.size() * sizeof(dyld_image_info)))
            {
                for(const auto & entry : list)
                    images.emplace_back(reinterpret_cast<ptr>(entry.imageLoadAddress), reinterpret_cast<ptr>(entry.imageFilePath));
            }
            // dyld is not in its own list.
            if(infos.dyldImageLoadAddress)
                images.emplace_back(reinterpret_cast<ptr>(infos.dyldImageLoadAddress), 0);
        }
        if(images.empty())
        {
            // dyld has not run yet: the kernel mapped only the executable and dyld.
            for(const auto & region : ReadMemoryMap(task))
            {
                mach_header_64 header = {};
                if((region.protection & VM_PROT_READ) && ReadMemory(task, region.base, &header, sizeof(header)) &&
                        header.magic == MH_MAGIC_64 && (header.filetype == MH_EXECUTE || header.filetype == MH_DYLINKER))
                    images.emplace_back(region.base, 0);
            }
        }

        std::vector<Module> modules;
        for(const auto & [base, pathAddress] : images)
        {
            if(std::any_of(modules.begin(), modules.end(), [base = base](const Module & module) { return module.base == base; }))
            continue;
            const auto cached = std::find_if(known.begin(), known.end(), [base = base](const Module & module)
            {
                return module.base == base;
            });
            Module module;
            if(cached != known.end())
                modules.push_back(*cached);
            else if(readModule(task, pid, base, pathAddress ? readString(task, pathAddress) : std::string(), module))
                modules.push_back(std::move(module));
        }
        std::sort(modules.begin(), modules.end(), [](const Module & a, const Module & b)
        {
            return a.base < b.base;
        });
        return modules;
    }
}
