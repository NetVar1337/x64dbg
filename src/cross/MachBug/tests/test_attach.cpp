#include "TestHarness.h"
#include <MachBug/process/ProcessList.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <csignal>

extern char** environ;

using namespace MachBug::test;

namespace
{
    // A target started by the test, so it can be attached to; killed if the test bails out.
    struct Target
    {
        pid_t pid = 0;

        Target()
        {
            const std::string path = FIXTURE("attach_target");
            char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
            REQUIRE(posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ) == 0);
        }

        ~Target()
        {
            if(pid > 0)
            {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
        }

        // Exit code once the debugger has let it go.
        int Wait()
        {
            int status = 0;
            REQUIRE(waitpid(pid, &status, 0) == pid);
            pid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        }
    };

    void AttachTo(RecordingDebugger & dbg, const Target & target)
    {
        REQUIRE(dbg.Attach(target.pid));
        dbg.StartOnThread();
        dbg.WaitFor(EventType::AttachBreakpoint);
        REQUIRE(dbg.First(EventType::CreateProcess).code == target.pid);
        REQUIRE(dbg.IsPaused());
        REQUIRE(dbg.Pc() != 0);
    }

    void Release(RecordingDebugger & dbg)
    {
        const int released = 1;
        REQUIRE(dbg.MemWrite(SymbolAddress("attach_target", "released", dbg.EntryPoint()), &released, sizeof(released)));
    }
}

TEST_CASE("Attach rejects processes it cannot debug", "[attach]")
{
    RecordingDebugger dbg;
    REQUIRE_FALSE(dbg.Attach(0));
    REQUIRE_FALSE(dbg.Attach(getpid()));
    REQUIRE_FALSE(dbg.Attach(99999999));
    REQUIRE(dbg.Count(EventType::InternalError) == 3);
}

TEST_CASE("Detach at a breakpoint lets the process finish on its own", "[attach]")
{
    Target target;
    RecordingDebugger dbg;
    AttachTo(dbg, target);
    Release(dbg);
    const ptr tick = SymbolAddress("attach_target", "tick", dbg.EntryPoint());
    REQUIRE(dbg.SetBreakpoint(tick));
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Breakpoint).pc == tick);

    // A breakpoint left behind would kill it with SIGTRAP.
    REQUIRE(dbg.Detach());
    dbg.WaitFor(EventType::Detach);
    dbg.Join();
    REQUIRE(dbg.Pid() == 0);
    REQUIRE(target.Wait() == 42);
}

TEST_CASE("Detach while the process runs", "[attach]")
{
    Target target;
    RecordingDebugger dbg;
    AttachTo(dbg, target);
    Release(dbg);
    REQUIRE(dbg.SetBreakpoint(SymbolAddress("attach_target", "main", dbg.EntryPoint())));
    dbg.Continue();
    std::this_thread::sleep_for(20ms);
    REQUIRE(dbg.Detach());
    dbg.WaitFor(EventType::Detach);
    dbg.Join();
    REQUIRE(target.Wait() == 42);
    REQUIRE(dbg.Count(EventType::InternalError) == 0);
}

TEST_CASE("A process can only be attached once", "[attach]")
{
    Target target;
    RecordingDebugger first;
    AttachTo(first, target);

    RecordingDebugger second;
    REQUIRE(second.Attach(target.pid));
    second.Start();
    REQUIRE(second.First(EventType::InternalError).message.find("already being debugged") != std::string::npos);

    // Stop kills an attached process too, like ElfBug.
    REQUIRE(first.Stop());
    REQUIRE(first.WaitFor(EventType::ExitProcess).code == -SIGKILL);
    first.Join();
    target.pid = 0;
}

TEST_CASE("The process list shows a target and whether it is traced", "[attach]")
{
    Target target;
    const auto find = [&]
    {
        const auto list = MachBug::ProcessList();
        const auto it = std::find_if(list.begin(), list.end(), [&](const MachBug::ProcessInfo & info)
        {
            return info.pid == target.pid;
        });
        REQUIRE(it != list.end());
        return *it;
    };

    const auto before = find();
    REQUIRE(before.name == "attach_target");
    REQUIRE(before.path == FIXTURE("attach_target"));
    REQUIRE(before.commandLine == FIXTURE("attach_target"));
    REQUIRE_FALSE(before.traced);
    REQUIRE_FALSE(before.translated);

    RecordingDebugger dbg;
    AttachTo(dbg, target);
    REQUIRE(find().traced);
    REQUIRE(dbg.Stop());
    dbg.WaitFor(EventType::ExitProcess);
    dbg.Join();
    target.pid = 0;
}
