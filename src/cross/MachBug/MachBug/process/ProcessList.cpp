#include <MachBug/process/ProcessList.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <cstring>

namespace MachBug
{
    namespace
    {
        // KERN_PROCARGS2 holds argc, the executable path, padding, then the arguments.
        std::string readCommandLine(const pid_t pid)
        {
            int mib[] = {CTL_KERN, KERN_PROCARGS2, pid};
            size_t size = 0;
            if(sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int))
                return {};
            std::vector<char> buffer(size);
            if(sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
                return {};

            int argc = 0;
            memcpy(&argc, buffer.data(), sizeof(argc));
            size_t offset = sizeof(argc);
            while(offset < size && buffer[offset] != '\0')
                ++offset;
            while(offset < size && buffer[offset] == '\0')
                ++offset;

            std::string commandLine;
            for(int i = 0; i < argc && offset < size; ++i)
            {
                const size_t length = strnlen(buffer.data() + offset, size - offset);
                if(!commandLine.empty())
                    commandLine += ' ';
                commandLine.append(buffer.data() + offset, length);
                offset += length + 1;
            }
            return commandLine;
        }

        bool isTranslated(const pid_t pid)
        {
            int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
            kinfo_proc info = {};
            size_t size = sizeof(info);
            return sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size == sizeof(info) &&
                   (info.kp_proc.p_flag & P_TRANSLATED) != 0;
        }
    }

    std::vector<ProcessInfo> ProcessList()
    {
        std::vector<pid_t> pids(1024);
        int count = 0;
        for(;;)
        {
            count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
            if(count < 0)
                return {};
            if(static_cast<size_t>(count) < pids.size())
                break;
            pids.resize(pids.size() * 2);
        }

        std::vector<ProcessInfo> processes;
        for(int i = 0; i < count; ++i)
        {
            // pid 0 is the kernel.
            if(pids[i] <= 0)
                continue;
            ProcessInfo info;
            info.pid = pids[i];

            proc_bsdinfo bsd = {};
            if(proc_pidinfo(info.pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) == sizeof(bsd))
                info.traced = (bsd.pbi_flags & PROC_FLAG_TRACED) != 0;

            char name[2 * MAXCOMLEN + 1] = {};
            if(proc_name(info.pid, name, sizeof(name)) > 0)
                info.name = name;
            char path[PROC_PIDPATHINFO_MAXSIZE] = {};
            if(proc_pidpath(info.pid, path, sizeof(path)) > 0)
                info.path = path;
            info.translated = isTranslated(info.pid);
            info.commandLine = readCommandLine(info.pid);
            processes.push_back(std::move(info));
        }
        return processes;
    }
}
