// A trap the program raises itself, not one of the debugger's breakpoints.
int main(void)
{
    __builtin_debugtrap();
    return 0;
}
