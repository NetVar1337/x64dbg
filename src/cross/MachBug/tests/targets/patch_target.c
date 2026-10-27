// Returns 7 unless the debugger changes value or identity's argument.
int value = 7;

__attribute__((noinline)) int identity(int argument)
{
    return argument;
}

int main(void)
{
    return identity(value);
}
