#include "slot.h"
#include "hw.h"

int slot_read(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len)
{
    DS_ID = id;
    DS_OFFSET = offset;
    DS_ADDR = addr;
    DS_LENGTH = len;
    DS_CTRL = DS_START;
    uint32_t t0 = rdcycle();
    while (DS_CTRL & DS_BUSY)
        if (rdcycle() - t0 > 20u * CPU_HZ) return -1;
    dcache_invalidate();
    return (int)((DS_CTRL >> 4) & 7);
}
