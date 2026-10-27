#include <signal.h>

static volatile sig_atomic_t handled = 0;

static void onSignal(int signal)
{
    handled = signal == SIGUSR1;
}

int main(void)
{
    signal(SIGUSR1, onSignal);
    raise(SIGUSR1);
    return handled ? 42 : 1;
}
