/*
 * DEBUG: 1 in a build for development (tools/fw.sh DEBUG=1, make DEBUG=1), 0 in a release (the default). A release runs none
 * of the measuring and diagnostic code: no timeline marks and counters read from the units every frame, no line of
 * statistics rewritten in debug.txt every frame, no race status block, no frame time display or benchmark, no marks or free
 * camera. What stays is what a player or a bug report needs: the boot log, the files, the settings read from screamer.cfg
 * and the simulation's own hooks (they run only in the simulation).
 */
#pragma once

#ifndef DEBUG
#define DEBUG 0
#endif
