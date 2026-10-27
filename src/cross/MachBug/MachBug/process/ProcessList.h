#pragma once

#include <sys/types.h>
#include <string>
#include <vector>

namespace MachBug
{
    struct ProcessInfo
    {
        pid_t pid = 0;
        bool traced = false;     // already being debugged
        bool translated = false; // runs under Rosetta, which this engine cannot debug
        std::string name;
        std::string path;
        std::string commandLine;
    };

    // Details of other users' processes are only readable as root; those stay empty.
    std::vector<ProcessInfo> ProcessList();
}
