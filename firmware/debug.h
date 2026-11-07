/*
 * DEBUG selects the development code. It is 1 in a build for development (tools/fw.sh DEBUG=1, or make
 * DEBUG=1) and 0 in a release, which is the default.
 *
 * A release leaves out the measuring and diagnostic code:
 *   - the timeline marks and the counters that are read from the units every frame
 *   - the line of statistics that is rewritten in debug.txt every frame
 *   - the race status block
 *   - the frame time display and the benchmark
 *   - the marks and the free camera
 * A release keeps what a player or a bug report needs: the boot log, the loading of the files and their
 * list, the settings read from screamer.cfg, and the simulation's own hooks, which run only in the
 * simulation.
 *
 * What the race measures and logs is in racedebug.c, which only a debug build compiles. racedebug.h gives a
 * release empty macros for the same calls, so that the race loop in main.c has no #if DEBUG.
 */
#pragma once

#ifndef DEBUG
#define DEBUG 0
#endif
