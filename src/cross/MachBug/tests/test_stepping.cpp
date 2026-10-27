#include "TestHarness.h"

using namespace MachBug::test;

TEST_CASE("Step into from a breakpoint and onwards", "[step]")
{
    RecordingDebugger dbg;
    Launch(dbg, "steps");
    const ptr entry = dbg.EntryPoint();
    REQUIRE(dbg.SetBreakpoint(entry));
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Breakpoint).pc == entry);

    // The first step also steps off the breakpoint.
    ptr previous = entry;
    for(int i = 0; i < 6; ++i)
    {
        dbg.StepInto();
        const auto step = dbg.WaitFor(EventType::Step);
        REQUIRE(step.pc != previous);
        previous = step.pc;
    }
    REQUIRE(dbg.HasBreakpoint(entry));
    REQUIRE(ContinueToExit(dbg) == 42);
    REQUIRE(dbg.Count(EventType::Breakpoint) == 1);
}

TEST_CASE("Step into from the system breakpoint", "[step]")
{
    RecordingDebugger dbg;
    Launch(dbg, "exit_code_42");
    const ptr start = dbg.Pc();
    dbg.StepInto();
    const auto step = dbg.WaitFor(EventType::Step);
    REQUIRE(step.pc != start);
    REQUIRE(ContinueToExit(dbg) == 42);
}
