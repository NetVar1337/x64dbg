#include <pthread.h>
#include <stddef.h>

static void* spin(void* arg)
{
    (void)arg;
    volatile unsigned long counter = 0;
    for(;;)
        ++counter;
    return NULL;
}

int main(void)
{
    pthread_t threads[3];
    for(int i = 0; i < 3; ++i)
        pthread_create(&threads[i], NULL, spin, NULL);
    spin(NULL);
}
