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

namespace
{
    // Stops at main of a fixture through a breakpoint that is then removed.
    void RunToMain(RecordingDebugger & dbg, const char* fixture)
    {
        Launch(dbg, fixture);
        REQUIRE(dbg.SetBreakpoint(dbg.EntryPoint()));
        dbg.Continue();
        REQUIRE(dbg.WaitFor(EventType::Breakpoint).pc == dbg.EntryPoint());
        REQUIRE(dbg.DeleteBreakpoint(dbg.EntryPoint()));
    }

    ptr Read(RecordingDebugger & dbg, const char* fixture, const char* symbol)
    {
        int value = 0;
        REQUIRE(dbg.MemRead(SymbolAddress(fixture, symbol, dbg.EntryPoint()), &value, sizeof(value)));
        return value;
    }
}

TEST_CASE("Step over runs calls instead of entering them", "[step]")
{
    RecordingDebugger dbg;
    RunToMain(dbg, "step_over_target");
    const ptr main = dbg.EntryPoint();
    const ptr leaf = SymbolAddress("step_over_target", "leaf", main);
    REQUIRE(leaf < main);

    // leaf and recurse sit below main; stepping over never lands in them.
    int steps = 0;
    for(ptr pc = dbg.Pc(); pc >= main && pc < main + 0x1000; pc = dbg.WaitFor(EventType::Step).pc, ++steps)
    {
        REQUIRE(steps < 100);
        dbg.StepOver();
    }
    REQUIRE(Read(dbg, "step_over_target", "unwound") == 3);
    for(const auto & event : dbg.Events())
    {
        if(event.type == EventType::Step)
            REQUIRE_FALSE((event.pc >= leaf && event.pc < main));
    }
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("Step into enters the call that step over runs", "[step]")
{
    RecordingDebugger dbg;
    RunToMain(dbg, "step_over_target");
    const ptr leaf = SymbolAddress("step_over_target", "leaf", dbg.EntryPoint());
    for(int steps = 0; dbg.Pc() != leaf; ++steps)
    {
        REQUIRE(steps < 100);
        dbg.StepInto();
        dbg.WaitFor(EventType::Step);
    }
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("Step over a recursive call stops in the same frame", "[step]")
{
    RecordingDebugger dbg;
    Launch(dbg, "step_over_target");
    const ptr recurse = SymbolAddress("step_over_target", "recurse", dbg.EntryPoint());
    REQUIRE(dbg.SetBreakpoint(recurse));
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Breakpoint).pc == recurse);
    REQUIRE(dbg.DeleteBreakpoint(recurse));

    // The outermost frame (depth 3) steps over its call; the two frames inside it return
    // to the same address first and must not end the step.
    for(int steps = 0; Read(dbg, "step_over_target", "unwound") == 0; ++steps)
    {
        REQUIRE(steps < 100);
        dbg.StepOver();
        dbg.WaitFor(EventType::Step);
    }
    REQUIRE(Read(dbg, "step_over_target", "unwound") == 2);
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("A breakpoint inside a stepped-over call stops there", "[step][breakpoint]")
{
    RecordingDebugger dbg;
    RunToMain(dbg, "step_over_target");
    const ptr leaf = SymbolAddress("step_over_target", "leaf", dbg.EntryPoint());
    REQUIRE(dbg.SetBreakpoint(leaf));
    for(int steps = 0; ; ++steps)
    {
        REQUIRE(steps < 100);
        dbg.StepOver();
        const auto event = dbg.WaitFor({EventType::Step, EventType::Breakpoint});
        if(event.type == EventType::Breakpoint)
        {
            REQUIRE(event.address == leaf);
            break;
        }
    }
    // The step over was abandoned: nothing else stops the program.
    REQUIRE(dbg.DeleteBreakpoint(leaf));
    REQUIRE(ContinueToExit(dbg) == 42);
    REQUIRE(dbg.Count(EventType::Breakpoint) == 2);
}

TEST_CASE("Other threads pass the step-over breakpoint unreported", "[step][threads]")
{
    RecordingDebugger dbg;
    Launch(dbg, "hit_threads");
    const ptr hit = SymbolAddress("hit_threads", "hit", dbg.EntryPoint());
    REQUIRE(dbg.SetBreakpoint(hit));
    dbg.Continue();
    const uint64_t stepper = dbg.WaitFor(EventType::Breakpoint).tid;
    REQUIRE(dbg.DeleteBreakpoint(hit));

    // Back in worker, every later call of hit is stepped over while the other threads
    // keep calling it and returning through the same step-over breakpoint.
    for(int steps = 0; steps < 40; ++steps)
    {
        dbg.StepOver();
        const auto step = dbg.WaitFor(EventType::Step);
        REQUIRE(step.tid == stepper);
        if(steps > 6)
            REQUIRE_FALSE(step.pc == hit);
    }
    REQUIRE(ContinueToExit(dbg) == 42);
}
