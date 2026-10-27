// main runs a handful of plain instructions to step through.
int main(void)
{
    volatile int value = 1;
    value += 2;
    value *= 3;
    value -= 2;
    value *= 6;
    return value;
}
