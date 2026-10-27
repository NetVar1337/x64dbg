// Ticks until the test sets released, then 300 more times, so a detach while it runs
// does not race its exit.
#include <unistd.h>

volatile int released = 0;

__attribute__((noinline)) void tick(void)
{
}

int main(void)
{
    for(int i = 0; i < 10000; ++i)
    {
        tick();
        usleep(1000);
        if(released && ++released > 300)
            return 42;
    }
    return 1;
}
