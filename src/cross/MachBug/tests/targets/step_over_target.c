// main calls leaf and a recursion; the test steps over both from main's first instruction.
volatile int unwound = 0;

__attribute__((noinline)) int leaf(int value)
{
    return value + 1;
}

__attribute__((noinline)) int recurse(int depth)
{
    if(depth == 0)
        return 0;
    int result = recurse(depth - 1);
    ++unwound; // counts the frames that returned so far
    return result + 1;
}

int main(void)
{
    int value = leaf(40);
    value += recurse(3);
    return value == 44 && unwound == 3 ? 42 : 1;
}
