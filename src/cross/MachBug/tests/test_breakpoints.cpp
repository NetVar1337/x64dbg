#include "TestHarness.h"
#include <array>

using namespace MachBug::test;

TEST_CASE("Breakpoint at the entry point is hit and stepped off", "[breakpoint]")
{
    RecordingDebugger dbg;
    Launch(dbg, "steps");
    const ptr entry = dbg.EntryPoint();
    REQUIRE(dbg.SetBreakpoint(entry));
    REQUIRE(dbg.HasBreakpoint(entry));

    dbg.Continue();
    const auto hit = dbg.WaitFor(EventType::Breakpoint);
    REQUIRE(hit.address == entry);
    REQUIRE(hit.pc == entry);

    REQUIRE(ContinueToExit(dbg) == 42);
    REQUIRE(dbg.Count(EventType::Breakpoint) == 1);
}

TEST_CASE("Memory reads show the code under a breakpoint", "[breakpoint][memory]")
{
    RecordingDebugger dbg;
    Launch(dbg, "steps");
    const ptr entry = dbg.EntryPoint();
    std::array<uint8_t, 16> before = {}, during = {}, after = {};
    REQUIRE(dbg.MemRead(entry, before.data(), before.size()));
    REQUIRE(dbg.SetBreakpoint(entry + MachBug::kBreakpointSize));
    REQUIRE(dbg.MemRead(entry, during.data(), during.size()));
    REQUIRE(dbg.DeleteBreakpoint(entry + MachBug::kBreakpointSize));
    REQUIRE_FALSE(dbg.HasBreakpoint(entry + MachBug::kBreakpointSize));
    REQUIRE(dbg.MemRead(entry, after.data(), after.size()));
    REQUIRE(before == during);
    REQUIRE(before == after);

    REQUIRE(ContinueToExit(dbg) == 42);
    REQUIRE(dbg.Count(EventType::Breakpoint) == 0);
}

TEST_CASE("Breakpoint hit by several threads", "[breakpoint][threads]")
{
    RecordingDebugger dbg;
    Launch(dbg, "hit_threads");
    const ptr hit = SymbolAddress("hit_threads", "hit", dbg.EntryPoint());
    REQUIRE(dbg.SetBreakpoint(hit));

    int hits = 0;
    for(;;)
    {
        dbg.Continue();
        const auto event = dbg.WaitFor({EventType::Breakpoint, EventType::ExitProcess});
        if(event.type == EventType::ExitProcess)
        {
            REQUIRE(event.code == 42);
            break;
        }
        REQUIRE(event.address == hit);
        REQUIRE(event.pc == hit);
        ++hits;
    }
    dbg.Join();
    REQUIRE(hits == 40);
}

TEST_CASE("Writing memory changes what the program reads", "[memory]")
{
    RecordingDebugger dbg;
    Launch(dbg, "patch_target");
    const ptr value = SymbolAddress("patch_target", "value", dbg.EntryPoint());
    int current = 0;
    REQUIRE(dbg.MemRead(value, &current, sizeof(current)));
    REQUIRE(current == 7);
    const int patched = 42;
    REQUIRE(dbg.MemWrite(value, &patched, sizeof(patched)));
    REQUIRE(ContinueToExit(dbg) == 42);
}

TEST_CASE("Writing a register changes the program's result", "[registers]")
{
    RecordingDebugger dbg;
    Launch(dbg, "patch_target");
    const ptr identity = SymbolAddress("patch_target", "identity", dbg.EntryPoint());
    REQUIRE(dbg.SetBreakpoint(identity));
    dbg.Continue();
    REQUIRE(dbg.WaitFor(EventType::Breakpoint).address == identity);
    REQUIRE(dbg.SetRegister(kFirstArgument, 42));
    REQUIRE_FALSE(dbg.SetRegister("no_such_register", 1));
    REQUIRE(ContinueToExit(dbg) == 42);
}
