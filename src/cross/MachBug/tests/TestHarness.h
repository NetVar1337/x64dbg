#pragma once

#include <MachBug/core/Debugger.h>
#include <catch2/catch_test_macros.hpp>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define FIXTURE(name) (std::string(MACHBUG_TESTS_TARGETS_DIR) + "/" + (name))

namespace MachBug::test
{
    using namespace std::chrono_literals;
    using MachBug::ptr;

    enum class EventType
    {
        CreateProcess,
        ExitProcess,
        CreateThread,
        ExitThread,
        SystemBreakpoint,
        AttachBreakpoint,
        Detach,
        Breakpoint,
        Step,
        Paused,
        Exception,
        InternalError,
    };

    struct Event
    {
        EventType type{};
        int code = 0;     // pid, exit code or signal
        ptr address = 0;  // entry point, breakpoint or fault address
        uint64_t tid = 0; // the thread created or exited
        ptr pc = 0;       // current thread at a stop
        std::string message;
    };

    class RecordingDebugger : public Debugger
    {
    public:
        ~RecordingDebugger() override
        {
            if(mLoop.joinable())
            {
                Stop();
                mLoop.join();
            }
        }

        void StartOnThread()
        {
            mLoop = std::thread([this] { Start(); });
        }

        void Join()
        {
            if(mLoop.joinable())
                mLoop.join();
        }

        // The first event of one of the types not returned by an earlier call.
        Event WaitFor(std::initializer_list<EventType> types, std::chrono::milliseconds timeout = 10s)
        {
            std::unique_lock lock(mMutex);
            const auto found = [&]
            {
                for(; mCursor < mEvents.size(); ++mCursor)
                {
                    for(const auto type : types)
                    {
                        if(mEvents[mCursor].type == type)
                            return true;
                    }
                }
                return false;
            };
            if(!mCv.wait_for(lock, timeout, found))
                FAIL("timed out waiting for an event, recorded: " << describe());
            return mEvents[mCursor++];
        }

        Event WaitFor(const EventType type, const std::chrono::milliseconds timeout = 10s)
        {
            return WaitFor({type}, timeout);
        }

        std::vector<Event> Events() const
        {
            std::lock_guard lock(mMutex);
            return mEvents;
        }

        size_t Count(const EventType type) const
        {
            std::lock_guard lock(mMutex);
            size_t count = 0;
            for(const auto & event : mEvents)
                count += event.type == type;
            return count;
        }

        // The first recorded event of the type, for checks after the fact.
        Event First(const EventType type) const
        {
            std::lock_guard lock(mMutex);
            for(const auto & event : mEvents)
            {
                if(event.type == type)
                    return event;
            }
            FAIL("no event of that type, recorded: " << describe());
            return {};
        }

        ptr Pc() const
        {
            ThreadState state = {};
            return GetRegisters(state) ? GetPc(state) : 0;
        }

    protected:
        void cbCreateProcess(const pid_t pid, const ptr entryPoint) override
        {
            record({EventType::CreateProcess, pid, entryPoint});
        }

        void cbExitProcess(const int exitCode) override
        {
            record({EventType::ExitProcess, exitCode});
        }

        void cbCreateThread(const uint64_t tid) override
        {
            record({EventType::CreateThread, 0, 0, tid});
        }

        void cbExitThread(const uint64_t tid) override
        {
            record({EventType::ExitThread, 0, 0, tid});
        }

        void cbSystemBreakpoint() override
        {
            record({EventType::SystemBreakpoint, 0, 0, 0, Pc()});
        }

        void cbAttachBreakpoint() override
        {
            record({EventType::AttachBreakpoint, 0, 0, 0, Pc()});
        }

        void cbDetach() override
        {
            record({EventType::Detach});
        }

        void cbBreakpoint(const ptr address) override
        {
            record({EventType::Breakpoint, 0, address, CurrentThreadId(), Pc()});
        }

        void cbStep() override
        {
            record({EventType::Step, 0, 0, CurrentThreadId(), Pc()});
        }

        void cbPaused() override
        {
            record({EventType::Paused, 0, 0, 0, Pc()});
        }

        void cbException(const int signal, const ptr address) override
        {
            record({EventType::Exception, signal, address, 0, Pc()});
        }

        void cbInternalError(const std::string & error) override
        {
            record({EventType::InternalError, 0, 0, 0, 0, error});
        }

    private:
        void record(const Event & event)
        {
            std::lock_guard lock(mMutex);
            mEvents.push_back(event);
            mCv.notify_all();
        }

        std::string describe() const
        {
            static const char* names[] = {"CreateProcess", "ExitProcess", "CreateThread", "ExitThread", "SystemBreakpoint",
                                          "AttachBreakpoint", "Detach", "Breakpoint", "Step", "Paused", "Exception", "InternalError"
                                         };
            std::ostringstream out;
            for(const auto & event : mEvents)
                out << names[static_cast<int>(event.type)] << "(" << event.code << std::hex << " 0x" << event.address << " pc 0x" << event.pc
                    << std::dec << ") " << event.message << "; ";
            return out.str();
        }

        std::thread mLoop;
        mutable std::mutex mMutex;
        std::condition_variable mCv;
        std::vector<Event> mEvents;
        size_t mCursor = 0;
    };

    // Runs the fixture up to its system breakpoint.
    inline void Launch(RecordingDebugger & dbg, const char* fixture)
    {
        REQUIRE(dbg.Init(FIXTURE(fixture).c_str()));
        dbg.StartOnThread();
        dbg.WaitFor(EventType::SystemBreakpoint);
    }

    inline int ContinueToExit(RecordingDebugger & dbg)
    {
        dbg.Continue();
        const int exitCode = dbg.WaitFor(EventType::ExitProcess).code;
        dbg.Join();
        return exitCode;
    }

    // Where a fixture's symbol is loaded, from its symbol table and the entry point (main).
    inline ptr SymbolAddress(const char* fixture, const std::string & symbol, const ptr entryPoint)
    {
        std::ifstream file(FIXTURE(fixture), std::ios::binary);
        const std::vector<char> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(image.size() >= sizeof(mach_header_64));
        const auto* header = reinterpret_cast<const mach_header_64*>(image.data());
        REQUIRE(header->magic == MH_MAGIC_64);

        std::map<std::string, uint64_t> symbols;
        size_t offset = sizeof(mach_header_64);
        for(uint32_t i = 0; i < header->ncmds; ++i)
        {
            const auto* command = reinterpret_cast<const load_command*>(image.data() + offset);
            if(command->cmd == LC_SYMTAB)
            {
                const auto* symtab = reinterpret_cast<const symtab_command*>(command);
                const auto* list = reinterpret_cast<const nlist_64*>(image.data() + symtab->symoff);
                for(uint32_t s = 0; s < symtab->nsyms; ++s)
                {
                    if((list[s].n_type & N_TYPE) == N_SECT)
                        symbols[image.data() + symtab->stroff + list[s].n_un.n_strx] = list[s].n_value;
                }
            }
            offset += command->cmdsize;
        }
        REQUIRE(symbols.count("_main"));
        REQUIRE(symbols.count("_" + symbol));
        return entryPoint - symbols["_main"] + symbols["_" + symbol];
    }

#if defined(__x86_64__)
    constexpr const char* kFirstArgument = "rdi";
#else
    constexpr const char* kFirstArgument = "x0";
#endif
}
