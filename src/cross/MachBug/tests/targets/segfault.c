#include "TargetUtil.h"

int main(void)
{
    *(volatile int*)kSegfaultAddress = 1;
    return 0;
}
