#include "TestHarness.h"
#include <algorithm>
#include <csignal>
#include <cstdlib>

using namespace MachBug::test;

namespace
{
    bool HasModule(const std::vector<MachBug::Module> & modules, const std::string & path)
    {
        return std::any_of(modules.begin(), modules.end(), [&](const MachBug::Module & module)
        {
            return module.path == path;
        });
    }
}

TEST_CASE("Before dyld runs, the executable and dyld are the modules", "[modules]")
{
    RecordingDebugger dbg;
    Launch(dbg, "exit_code_42");
    const auto modules = dbg.Modules();
    REQUIRE(modules.size() == 2);
    REQUIRE(HasModule(modules, FIXTURE("exit_code_42")));
    REQUIRE(HasModule(modules, "/usr/lib/dyld"));

    MachBug::Module module;
    REQUIRE(dbg.ModuleFromAddress(dbg.EntryPoint(), module));
    REQUIRE(module.path == FIXTURE("exit_code_42"));
    REQUIRE(module.base < dbg.EntryPoint());
    // The system breakpoint is in dyld.
    REQUIRE(dbg.ModuleFromAddress(dbg.Pc(), module));
    REQUIRE(module.path == "/usr/lib/dyld");
    REQUIRE_FALSE(dbg.ModuleFromAddress(0, module));
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("At main, dyld's image list has the system libraries", "[modules]")
{
    RecordingDebugger dbg;
    Launch(dbg, "exit_code_42");
    REQUIRE(dbg.SetBreakpoint(dbg.EntryPoint()));
    dbg.Continue();
    dbg.WaitFor(EventType::Breakpoint);

    const auto modules = dbg.Modules();
    REQUIRE(modules.size() > 10);
    REQUIRE(HasModule(modules, FIXTURE("exit_code_42")));
    REQUIRE(HasModule(modules, "/usr/lib/libSystem.B.dylib"));
    REQUIRE(HasModule(modules, "/usr/lib/dyld"));
    REQUIRE(std::is_sorted(modules.begin(), modules.end(), [](const MachBug::Module & a, const MachBug::Module & b)
    {
        return a.base < b.base;
    }));

    // The shared cache sits at the same address in every process of a boot, ours included.
    MachBug::Module module;
    REQUIRE(dbg.ModuleFromAddress(reinterpret_cast<MachBug::ptr>(&abort), module));
    REQUIRE(module.path == "/usr/lib/system/libsystem_c.dylib");
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("A library loaded later is listed at the next stop", "[modules]")
{
    RecordingDebugger dbg;
    Launch(dbg, "dlopen_target");
    REQUIRE_FALSE(HasModule(dbg.Modules(), "/usr/lib/libz.1.dylib"));
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Exception).code == SIGTRAP);
    REQUIRE(HasModule(dbg.Modules(), "/usr/lib/libz.1.dylib"));
    REQUIRE(dbg.Stop());
    dbg.WaitFor(EventType::ExitProcess);
    dbg.Join();
}
