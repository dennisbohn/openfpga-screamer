/*
 * The saves (save.h): CHOICE.DAT and ATTACK.DAT read at boot, their sizes into the slot table.
 */
#include "save.h"
#include "hw.h"
#include "champ.h"
#include "slot.h"
#include "dlog.h"
#include <string.h>

/* a slot of exactly size bytes into the SDRAM at addr: 1 if the APF read it */
static int load(uint32_t id, uint32_t addr, uint32_t size)
{
    return slot_size(id) == size && slot_read(id, 0, addr, size) == 0;
}

/* CHOICE.DAT as the game writes it: the name three capitals, the options, the championship's
   stages and the unlocks within their ranges */
static int choice_ok(void)
{
    for(int k = 0; k < 3; k++)
        if(choice.name[k] < 'A' || choice.name[k] > 'Z') return 0;
    return choice.name[3] == 0 && (uint32_t)choice.detail <= 2 && (uint32_t)choice.difficulty <= 4 &&
           (uint32_t)choice.laps <= 3 && (uint32_t)choice.stage <= 52 && (uint32_t)choice.prev <= 52 &&
           (uint32_t)choice.difficulties - 2 <= 1 && (uint32_t)choice.car < 14 && (uint32_t)choice.track < 12 &&
           (uint32_t)choice.bullet <= 1 && (uint32_t)choice.level <= 9 && (uint32_t)choice.level_shown <= 9;
}

void save_load(void)
{
    int c = load(SAVE_CHOICE_SLOT, SAVE_CHOICE_ADDR, CHOICE_SIZE) && choice_ok();
    if(!c) choice_defaults();
    int a = load(SAVE_ATTACK_SLOT, SAVE_ATTACK_ADDR, SAVE_ATTACK_SIZE);
    if(!a) memset((void *)(uintptr_t)SAVE_ATTACK_ADDR, 0, SAVE_ATTACK_SIZE);
    slot_set_size(SAVE_CHOICE_SLOT, CHOICE_SIZE);
    slot_set_size(SAVE_ATTACK_SLOT, SAVE_ATTACK_SIZE);
    dlog_str(c ? "CHOICE.DAT loaded" : "CHOICE.DAT: the game's defaults");
    dlog_str(a ? ", ATTACK.DAT loaded\n" : ", ATTACK.DAT empty\n");
}
