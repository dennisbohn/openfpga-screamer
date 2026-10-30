#include "slot.h"
#include "hw.h"

void slot_start(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len)
{
    DS_ID = id;
    DS_OFFSET = offset;
    DS_ADDR = addr;
    DS_LENGTH = len;
    DS_CTRL = DS_START;
}

int slot_busy(void)
{
    return (DS_CTRL & DS_BUSY) != 0;
}

int slot_result(void)
{
    return (int)((DS_CTRL >> 4) & 7);
}

int slot_read(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len)
{
    slot_start(id, offset, addr, len);
    uint32_t t0 = rdcycle();
    while (slot_busy())
        if (rdcycle() - t0 > 20u * CPU_HZ) return -1;
    dcache_invalidate();
    return slot_result();
}

void slot_set_size(uint32_t id, uint32_t size)
{
    DS_ID = id;
    DS_SIZE = size;                 /* the look-up writes it into the slot's size word */
    while (DS_CTRL & DS_SIZING)
        ;
}

uint32_t slot_size(uint32_t id)
{
    DS_ID = id;                     /* starts the look-up in the slot table, a few hundred clocks */
    while (DS_CTRL & DS_SIZING)
        ;
    return DS_SIZE;
}
