int main(void)
{
    volatile unsigned long counter = 0;
    for(;;)
        ++counter;
}
