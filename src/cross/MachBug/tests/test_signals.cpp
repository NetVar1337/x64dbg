#include "TestHarness.h"
#include "targets/TargetUtil.h"
#include <csignal>

using namespace MachBug::test;

TEST_CASE("A fault is reported with its address and then kills the process", "[signal]")
{
    RecordingDebugger dbg;
    Launch(dbg, "segfault");
    dbg.Continue();
    const auto fault = dbg.WaitFor(EventType::Exception);
    REQUIRE(fault.code == SIGSEGV);
    REQUIRE(fault.address == kSegfaultAddress);
    REQUIRE(ContinueToExit(dbg) == -SIGSEGV);
    // The signal the fault turns into is not a second stop.
    REQUIRE(dbg.Count(EventType::Exception) == 1);
}

TEST_CASE("A signal is reported and then delivered to the handler", "[signal]")
{
    RecordingDebugger dbg;
    Launch(dbg, "signal_handler");
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Exception).code == SIGUSR1);
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("The program's own trap is reported as SIGTRAP", "[signal]")
{
    RecordingDebugger dbg;
    Launch(dbg, "trap");
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Exception).code == SIGTRAP);
    REQUIRE(ContinueToExit(dbg) == -SIGTRAP);
    REQUIRE(dbg.Count(EventType::Breakpoint) == 0);
}
