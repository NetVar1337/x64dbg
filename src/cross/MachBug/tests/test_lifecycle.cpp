#include "TestHarness.h"
#include <csignal>

using namespace MachBug::test;

TEST_CASE("Init fails for a missing binary", "[init]")
{
    RecordingDebugger dbg;
    REQUIRE_FALSE(dbg.Init("/nonexistent/machbug_fixture"));
    REQUIRE(dbg.Count(EventType::InternalError) == 1);
}

TEST_CASE("Start without Init reports an error", "[init]")
{
    RecordingDebugger dbg;
    dbg.Start();
    REQUIRE(dbg.First(EventType::InternalError).message.find("without Init") != std::string::npos);
}

TEST_CASE("Launch stops before the first instruction and runs to exit", "[process]")
{
    RecordingDebugger dbg;
    Launch(dbg, "exit_code_42");
    const auto created = dbg.First(EventType::CreateProcess);
    REQUIRE(created.code > 0);
    REQUIRE(created.address == dbg.EntryPoint());
    REQUIRE(dbg.EntryPoint() != 0);
    REQUIRE(dbg.IsPaused());
    REQUIRE(dbg.Pid() == created.code);
    REQUIRE(dbg.ThreadIds().size() == 1);
    REQUIRE(dbg.CurrentThreadId() == dbg.ThreadIds().front());
    REQUIRE(dbg.Pc() != 0);

    REQUIRE(ContinueToExit(dbg) == 42);
    REQUIRE(dbg.Pid() == 0);
    REQUIRE(dbg.Count(EventType::InternalError) == 0);
}

TEST_CASE("Stop at the system breakpoint kills the process", "[control]")
{
    RecordingDebugger dbg;
    Launch(dbg, "exit_code_42");
    REQUIRE(dbg.Stop());
    REQUIRE(dbg.WaitFor(EventType::ExitProcess).code == -SIGKILL);
    dbg.Join();
}

TEST_CASE("Stop kills a running process", "[control]")
{
    RecordingDebugger dbg;
    Launch(dbg, "run_endlessly");
    dbg.Continue();
    std::this_thread::sleep_for(100ms);
    REQUIRE_FALSE(dbg.IsPaused());
    REQUIRE(dbg.Stop());
    REQUIRE(dbg.WaitFor(EventType::ExitProcess).code == -SIGKILL);
    dbg.Join();
}

TEST_CASE("Pause stops a running process and Continue resumes it", "[control]")
{
    RecordingDebugger dbg;
    Launch(dbg, "run_endlessly");
    for(int round = 0; round < 3; ++round)
    {
        dbg.Continue();
        std::this_thread::sleep_for(50ms);
        dbg.Pause();
        const auto paused = dbg.WaitFor(EventType::Paused);
        REQUIRE(dbg.IsPaused());
        REQUIRE(paused.pc != 0);
    }
    REQUIRE(dbg.Stop());
    REQUIRE(dbg.WaitFor(EventType::ExitProcess).code == -SIGKILL);
    dbg.Join();
}
