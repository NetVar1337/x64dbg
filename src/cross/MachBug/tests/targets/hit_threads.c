// Four threads call hit() ten times each; the test counts the breakpoint hits.
#include <pthread.h>
#include <stddef.h>

__attribute__((noinline)) int hit(int value)
{
    return value + 1;
}

static void* worker(void* arg)
{
    int* sum = arg;
    for(int i = 0; i < 10; ++i)
        *sum += hit(i);
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int sums[4] = {0};
    for(int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, worker, &sums[i]);
    for(int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    for(int i = 0; i < 4; ++i)
    {
        if(sums[i] != 55)
            return 1;
    }
    return 42;
}
