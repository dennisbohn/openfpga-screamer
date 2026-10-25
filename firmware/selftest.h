/*
 * CPU self-tests of step 1: SYS_SIM bit 0 (integer), bit 1 (floating point), bit 2 (CFU).
 * Each returns 1 on pass and logs its first failures to the debug log.
 */
#pragma once

int selftest_int(void);
int selftest_float(void);
int selftest_cfu(void);
