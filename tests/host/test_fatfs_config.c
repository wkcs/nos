/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include "../../fs/fatfs/ff.h"

int main(void)
{
    assert(_FS_TINY == 1);
    assert(_FS_REENTRANT == 1);
    assert(_MAX_SS == 4096);
    assert(sizeof(FIL) < _MAX_SS / 2);
    return 0;
}
