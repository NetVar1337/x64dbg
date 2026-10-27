#include "TestHarness.h"
#include <csignal>
#include <set>

using namespace MachBug::test;

TEST_CASE("Threads are listed at a stop and can be switched to", "[threads]")
{
    RecordingDebugger dbg;
    Launch(dbg, "threads_spin");
    dbg.Continue();
    std::this_thread::sleep_for(200ms);
    dbg.Pause();
    dbg.WaitFor(EventType::Paused);

    const auto tids = dbg.ThreadIds();
    REQUIRE(tids.size() == 4);
    REQUIRE(std::set<uint64_t>(tids.begin(), tids.end()).size() == 4);
    REQUIRE(dbg.Count(EventType::CreateThread) == 3);

    for(const uint64_t tid : tids)
    {
        REQUIRE(dbg.SwitchThread(tid));
        REQUIRE(dbg.CurrentThreadId() == tid);
        REQUIRE(dbg.Pc() != 0);
    }
    REQUIRE_FALSE(dbg.SwitchThread(1));

    REQUIRE(dbg.Stop());
    REQUIRE(dbg.WaitFor(EventType::ExitProcess).code == -SIGKILL);
    dbg.Join();
}
