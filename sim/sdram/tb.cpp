// SDRAM random test: the front end and the controller (docs/memory.md), that is sdram_port.v and
// sdram.v on the SDRAM model, under random traffic, with a shadow memory as the reference.
//
// Usage: Vsdram_system [seed, default 1] [clocks, default 2000000]    (tools/sim.sh sdram)
// Prints the seed, how often each case of the coverage list happened, the model's error count and
// longest refresh gap, and PASS or FAIL. The exit status is 0 or 1. Up to 30 errors are printed,
// each with the clock it happened at.
//
// How it is driven
//   - Bus models for the instruction bus (iBus: lines of 8 words) and the data bus (dBus: single
//     words, lines, and writes with byte masks) follow VexRiscv's cache-bus protocol.
//   - A bridge writer writes its own region at least 88 clocks apart.
//   - CPU resets of 1 to 60 clocks and reinit pulses (the short initialisation after a clock phase
//     step) fall at random times.
//   - Addresses come from a small pool: a few lines in a few rows of every bank, row and bank
//     boundaries, and the last line of the memory. So reads keep hitting words that writes have
//     changed a short time before. The dBus follows a write with a read of the same line now and
//     then.
//   - Gaps between requests are mostly 0 to 3 clocks, and the iBus and the dBus often handshake in
//     the same clock.
//   - At the end a fast bridge burst must set the overflow bit, and Reset Enter must clear it.
//
// Checks
//   - Words come only for requests, in the right number, never in the handshake clock.
//   - Data equals the shadow memory under the ordering rule: a dBus read sees every write taken
//     before it; an iBus line may also see writes taken in its handshake clock or after.
//   - The READ and WRITE commands at the SDRAM pins follow the jobs in the order the controller
//     took them, at the right columns and rows.
//   - cpu_writes_idle never reads 1 while a CPU write is not done.
//   - The model reports no error and no refresh gap above 580 clocks.
//   - Every case in the coverage list (need[] in main) happened at least once.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Vsdram_system.h"

// The random numbers: a xorshift generator. below(n) is an integer from 0 to n - 1, chance(p) is
// true p times in 1000, gap() is the idle time after a request in clocks (0 to 3 in 80 % of the
// cases, else 0 to 39).
static uint64_t rng_state;
static uint32_t rnd() {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return uint32_t(rng_state >> 16);
}
static uint32_t below(uint32_t n) { return rnd() % n; }
static bool chance(uint32_t per_1000) { return below(1000) < per_1000; }
static uint32_t gap() { return chance(800) ? below(4) : below(40); }

static Vsdram_system top;
static uint64_t cycle = 0;
static int errors = 0;
// ERR(format, ...) counts an error and prints it with the clock. Only the first 30 are printed;
// at 1000 errors the run stops.
#define ERR(...) do { if (errors++ < 30) { printf("clock %llu: ", (unsigned long long)cycle); \
                                           printf(__VA_ARGS__); printf("\n"); } \
                      if (errors >= 1000) { printf("FAIL: 1000 errors, stopped\n"); exit(1); } } while (0)
// Watchdog: a request without an answer, or a write that never reaches the pins, would hang the
// CPU. The longest legal pause is a refresh or a reinit, well below this many clocks.
static const uint64_t STALL_LIMIT = 20000;
static uint64_t last_progress = 0;

// The reference: a shadow of the CPU region, word index -> value. Words that are not in it are
// unknown and not compared. wlog is the log of the CPU writes with the clock they were taken, for
// iBus reads, which may see a write that comes after their handshake.
static std::map<uint32_t, uint32_t> shadow;
struct WriteLog { uint64_t at; uint32_t word, value; };
static std::deque<WriteLog> wlog;

static std::vector<uint32_t> pool;          // line base word indices for CPU traffic
static std::vector<uint32_t> br_pool;       // line base word indices for the bridge
static std::map<uint32_t, uint32_t> br_shadow;

// coverage: how often each case happened, by the name used in need[] in main
static std::map<std::string, uint64_t> cov;

// The SDRAM commands that must follow, derived from the jobs the controller took (take_job) and
// compared with the commands seen at the pins (see_command). open_row is the row each bank has
// open, as seen at the pins (-1: closed).
struct Cmd { bool write; int bank; int row; int col; };
static std::deque<Cmd> expect_cmds;
static int open_row[4] = { -1, -1, -1, -1 };
static bool traffic = false;                // the random traffic has started
// CPU writes taken but not yet done at the pins. halves_left is the number of WRITE commands
// (one per 16-bit half) still to come; taken is set when the controller has taken the job.
struct PendingWrite { uint32_t word; int halves_left; bool taken; };
static std::deque<PendingWrite> pending_writes;

// Random write data for a byte mask: a one-byte mask gets one random byte repeated in all four
// lanes, a half-word mask one half-word repeated in both halves, any other mask random bits.
static uint32_t lane_data(uint32_t mask) {
    uint32_t b = rnd() & 0xFF, h = rnd() & 0xFFFF;
    switch (mask) {
    case 0x1: case 0x2: case 0x4: case 0x8: return b * 0x01010101u;
    case 0x3: case 0xC: return h * 0x00010001u;
    default: return rnd();
    }
}
static const uint32_t masks[7] = { 0x1, 0x2, 0x4, 0x8, 0x3, 0xC, 0xF };
// The 32-bit mask with 0xFF in each byte that the 4-bit byte mask selects.
static uint32_t bytes_of(uint32_t mask) {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++) if (mask >> i & 1) m |= 0xFFu << (8 * i);
    return m;
}

// --- bus models ---
// The iBus fetches lines of 8 words. valid: a request is waiting for its handshake. busy: the
// handshake is done and the 8 words are to come (hs_at is the clock of the handshake, got the
// words received, line_w the line's first word index). before[] and known[] are the shadow's
// words at the handshake. idle_until is the clock from which the next request may start.
struct IBus {
    bool valid = false; uint32_t line = 0;
    bool busy = false; uint64_t hs_at = 0;
    int got = 0; uint32_t line_w = 0;
    uint32_t before[8]; bool known[8];
    uint64_t idle_until = 0;
} ib;

// The dBus does single reads (want = 1), line reads (want = 8) and writes with a byte mask. For a
// read, expect[] and known[] are the shadow's words at the handshake, word_hs the first word's
// index. last_write_* is the line and the clock of the last write, so that a read can follow it.
struct DBus {
    bool valid = false, wr = false, line = false; uint32_t word = 0, data = 0, mask = 0;
    bool busy = false; uint64_t hs_at = 0; int want = 0, got = 0; uint32_t word_hs = 0;
    uint32_t expect[8]; bool known[8];
    uint64_t idle_until = 0;
    uint32_t last_write_line = 0; uint64_t last_write_at = 0;
} db;

// Starts an iBus request for a random line of the pool.
static void ib_new() {
    ib.line = pool[below(pool.size())];
    ib.valid = true;
}

// Starts a dBus request: 40 % write, 30 % line read, 30 % single read. A request that is not a
// write is a line read if a write came less than 4 clocks before, and then in 70 % of the cases
// of the line of that write.
static void db_new() {
    uint32_t r = below(100);
    uint32_t line = pool[below(pool.size())];
    if (r < 40) {                               // write
        db.wr = true; db.line = false;
        db.word = line + below(8);
        db.mask = masks[below(7)];
        db.data = lane_data(db.mask);
    } else if (r < 70 || (db.last_write_at && cycle - db.last_write_at < 4)) {
        db.wr = false; db.line = true;          // line read, often of the line of the last write
        db.word = (db.last_write_at && cycle - db.last_write_at < 4 && chance(700))
                  ? db.last_write_line : line;
        db.mask = masks[below(7)];              // the data cache sends the lanes of the load that missed
    } else {
        db.wr = false; db.line = false;
        db.word = line + below(8);
        db.mask = masks[below(7)];
    }
    db.valid = true;
}

// Puts the requests of the bus models on the pins of the design.
static void drive() {
    top.ib_cmd_valid = ib.valid;
    top.ib_cmd_addr = ib.line >> 3;
    top.db_cmd_valid = db.valid;
    top.db_cmd_wr = db.wr;
    top.db_cmd_line = db.line;
    top.db_cmd_addr = db.line ? (db.word & ~7u) : db.word;
    top.db_cmd_data = db.data;
    top.db_cmd_mask = db.mask;
}

// Called at a CPU reset. A reset may drop a write that is still in the port register and that the
// controller has not taken: its word becomes unknown in the shadow, and the write is forgotten.
static void forget_pending(bool reset) {
    for (auto &p : pending_writes)
        if (!p.taken && reset) { shadow.erase(p.word); cov["reset with a write queued"]++; }
    std::deque<PendingWrite> keep;
    for (auto &p : pending_writes) if (p.taken) keep.push_back(p);
    pending_writes = keep;
}

// One word has arrived on the iBus. There must be a request, the word must not come in the
// handshake clock, the error flag must be low, and the data must equal the shadow's word at the
// handshake or a write that was taken later.
static void check_ib_word(uint32_t data, bool error) {
    if (!ib.busy) { ERR("iBus word without a request"); return; }
    if (cycle <= ib.hs_at) ERR("iBus word in the handshake clock");
    if (error) ERR("iBus word with the error flag");
    int i = ib.got++;
    uint32_t w = ib.line_w + i;
    if (ib.known[i] && data != ib.before[i]) {
        bool later = false;
        for (auto &e : wlog) if (e.word == w && e.at >= ib.hs_at && e.value == data) later = true;
        if (!later)
            ERR("iBus word %u of line %06x: %08x, expected %08x", i, ib.line_w << 2, data, ib.before[i]);
        else cov["iBus saw a later write"]++;
    }
    if (ib.got == 8) { ib.busy = false; ib.idle_until = cycle + gap(); }
}

// The same for one word on the dBus. last is the flag the design sets with the last word of the
// request; it must be set exactly then.
static void check_db_word(uint32_t data, bool error, bool last) {
    if (!db.busy) { ERR("dBus word without a request"); return; }
    if (cycle <= db.hs_at) ERR("dBus word in the handshake clock");
    if (error) ERR("dBus word with the error flag");
    int i = db.got++;
    if (db.known[i] && data != db.expect[i])
        ERR("dBus %s word %d at %06x: %08x, expected %08x", db.want == 8 ? "line" : "single", i,
            (db.word_hs + i) << 2, data, db.expect[i]);
    if ((db.got == db.want) != last) ERR("dBus last flag wrong on word %d", i);
    if (db.got == db.want) { db.busy = false; db.idle_until = cycle + gap(); }
}

// The controller takes a job (job_take) at the coming edge. This queues the SDRAM commands the
// job must lead to: one WRITE per 16-bit half that the byte mask selects, 8 READs for a line (one
// per word), one READ for a single word. A word is two halves; col2 is the SDRAM column of the
// first one. Port 1 is the dBus: when it takes a CPU write, that write is marked as taken.
static void take_job() {
    uint32_t a = top.job_addr;              // word index
    int bank = (a >> 9) & 3, row = (a >> 11) & 0x1FFF, col2 = (a & 0x1FF) << 1;
    int port = top.job_tag & 3;
    if (top.job_write) {
        uint32_t m = top.job_mask;
        int n = 0;
        if (m & 3) { expect_cmds.push_back({ true, bank, row, col2 }); n++; }
        if (m & 12) { expect_cmds.push_back({ true, bank, row, col2 | 1 }); n++; }
        if (port == 1)
            for (auto &p : pending_writes)
                if (!p.taken && p.word == a) { p.taken = true; p.halves_left = n; break; }
    } else if (top.job_line) {
        for (int w = 0; w < 8; w++) expect_cmds.push_back({ false, bank, row, (col2 & ~15) | (w << 1) });
    } else {
        expect_cmds.push_back({ false, bank, row, col2 });
    }
}

// Decodes the command on the SDRAM pins (the one launched at the last edge). ACTIVATE and
// PRECHARGE keep open_row up to date, and refresh, PRECHARGE and LOAD MODE count coverage cases.
// A READ or WRITE must be the next command that take_job queued, in the bank and column given
// and in the row that is open.
static void see_command() {
    int ras = top.dram_ras_n, cas = top.dram_cas_n, we = top.dram_we_n;
    int bank = top.dram_ba, a = top.dram_a;
    if (!ras && cas && we) { open_row[bank] = a; return; }                      // ACTIVATE
    if (!ras && cas && !we) {                                                   // PRECHARGE
        if (a & 0x400) for (int b = 0; b < 4; b++) open_row[b] = -1; else open_row[bank] = -1;
        if (!expect_cmds.empty()) cov["precharge between jobs"]++;
        return;
    }
    if (!ras && !cas && we) {                                                   // AUTO REFRESH
        if (!expect_cmds.empty() || ib.busy || db.busy) cov["refresh during traffic"]++;
        return;
    }
    if (!ras && !cas && !we) { if (traffic) cov["mode registers written again"]++; return; }    // LOAD MODE
    if (ras && !cas) {                                                          // READ / WRITE
        bool write = !we;
        last_progress = cycle;
        if (expect_cmds.empty()) { ERR("%s at the pins without a job", write ? "WRITE" : "READ"); return; }
        Cmd c = expect_cmds.front(); expect_cmds.pop_front();
        if (c.write != write || c.bank != bank || c.col != (a & 0x3FF) || c.row != open_row[bank])
            ERR("%s bank %d row %d col %d, expected %s bank %d row %d col %d", write ? "WRITE" : "READ", bank,
                open_row[bank], a & 0x3FF, c.write ? "WRITE" : "READ", c.bank, c.row, c.col);
        if (write) {
            for (auto it = pending_writes.begin(); it != pending_writes.end(); ++it)
                if (it->taken &&
                    it->word == ((uint32_t(open_row[bank]) << 11) | (bank << 9) | ((a & 0x3FF) >> 1))) {
                    if (--it->halves_left == 0) pending_writes.erase(it);
                    break;
                }
        }
    }
}

// One clock. First, with the clock low, the pins are looked at: the command launched at the last
// edge, the handshakes and the job the controller takes at the coming edge. Then the rising edge
// follows and the words that arrive are checked. Last, the bus models and the shadow are updated
// for the handshakes that happened.
static void edge() {
    // clock low: the inputs for the coming edge are set and the combinational outputs settle
    top.clk = 0; top.eval();
    see_command();                          // the command that left at the last edge
    bool ib_hs = top.ib_cmd_valid && top.ib_cmd_ready;
    bool db_hs = top.db_cmd_valid && top.db_cmd_ready;
    if (top.job_take) take_job();
    if (top.cpu_writes_idle && !pending_writes.empty()) ERR("cpu_writes_idle with a CPU write not done");
    if (ib_hs && db_hs) cov["iBus and dBus in the same clock"]++;

    top.clk = 1; top.eval();
    cycle++;
    if (top.ib_rsp_valid) check_ib_word(top.ib_rsp_data, top.ib_rsp_error);
    if (top.db_rsp_valid) check_db_word(top.db_rsp_data, top.db_rsp_error, top.db_rsp_last);
    if (ib_hs || db_hs || top.ib_rsp_valid || top.db_rsp_valid) last_progress = cycle;
    if (!(ib.valid || ib.busy || db.valid || db.busy || !pending_writes.empty())) last_progress = cycle;
    else if (cycle - last_progress > STALL_LIMIT) {
        printf("FAIL: clock %llu: no progress for %llu clocks with a request or write open\n",
               (unsigned long long)cycle, (unsigned long long)STALL_LIMIT);
        exit(1);
    }

    if (ib_hs) {
        ib.valid = false; ib.busy = true; ib.hs_at = cycle; ib.got = 0; ib.line_w = ib.line;
        for (int i = 0; i < 8; i++) {
            auto it = shadow.find(ib.line + i);
            ib.known[i] = it != shadow.end();
            ib.before[i] = ib.known[i] ? it->second : 0;
        }
        cov["iBus line"]++;
    }
    if (db_hs) {
        db.valid = false;
        if (db.wr) {
            uint32_t m = bytes_of(db.mask);
            auto it = shadow.find(db.word);
            if (it != shadow.end()) it->second = (it->second & ~m) | (db.data & m);
            else if (db.mask == 0xF) shadow[db.word] = db.data;
            if (shadow.count(db.word)) wlog.push_back({ cycle, db.word, shadow[db.word] });
            pending_writes.push_back({ db.word, 0, false });
            db.last_write_line = db.word & ~7u; db.last_write_at = cycle;
            cov[std::string("write mask ") + "0123456789ABCDEF"[db.mask]]++;
            db.idle_until = cycle + gap();
        } else {
            db.busy = true; db.hs_at = cycle; db.got = 0; db.want = db.line ? 8 : 1;
            db.word_hs = db.line ? (db.word & ~7u) : db.word;
            for (int i = 0; i < db.want; i++) {
                auto it = shadow.find(db.word_hs + i);
                db.known[i] = it != shadow.end();
                db.expect[i] = db.known[i] ? it->second : 0;
            }
            if (db.line && db.last_write_at && db.hs_at - db.last_write_at <= 4 &&
                db.word_hs == db.last_write_line)
                cov["line read right after a write to it"]++;
            cov[db.line ? "dBus line" : "dBus single"]++;
        }
    }
    // keep the write log short: only what an outstanding iBus read may still see
    while (!wlog.empty() && (!ib.busy || wlog.front().at < ib.hs_at) && wlog.size() > 64) wlog.pop_front();
}

int main(int argc, char **argv) {
    uint32_t seed = argc > 1 ? uint32_t(atoi(argv[1])) : 1;
    uint64_t clocks = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2000000;
    rng_state = 0x9E3779B97F4A7C15ull ^ (uint64_t(seed) * 0x100000001B3ull);
    printf("seed %u, %llu clocks\n", seed, (unsigned long long)clocks);

    // The pool of lines for CPU traffic: 3 random rows of every bank with 3 random lines each, the
    // last line of each of those rows (a row and bank boundary), and the last line of the SDRAM.
    // The bridge has its own pool: 16 lines in rows 8000 to 8015, one bank after the other.
    for (int bank = 0; bank < 4; bank++)
        for (int r = 0; r < 3; r++) {
            uint32_t row = below(8192);
            for (int l = 0; l < 3; l++) pool.push_back((row << 11) | (bank << 9) | (below(64) << 3));
            pool.push_back((row << 11) | (bank << 9) | (63u << 3));     // last line of the row
        }
    pool.push_back((1u << 24) - 8);                                    // the last line of the SDRAM
    for (int i = 0; i < 16; i++) br_pool.push_back(((8000u + i) << 11) | ((i & 3) << 9) | (below(64) << 3));

    top.clk = 0; top.cpu_reset = 0; top.reinit = 0; top.br_wr = 0;
    drive();
    while (!top.sdram_ready) { edge(); if (cycle > 40000) { printf("FAIL: SDRAM never ready\n"); return 1; } }
    printf("SDRAM ready after %llu clocks\n", (unsigned long long)cycle);

    // Write every pool word once with a full mask, so that the shadow knows it and reads can be
    // checked from the start.
    for (uint32_t line : pool)
        for (int w = 0; w < 8; w++) {
            db.wr = true; db.line = false; db.word = line + w; db.mask = 0xF; db.data = rnd();
            db.valid = true;
            drive();
            while (db.valid) { edge(); drive(); }
        }
    while (!pending_writes.empty()) { edge(); drive(); }

    // The random traffic. In every clock: maybe a reinit pulse, maybe a CPU reset (which drops the
    // requests of both bus models and the writes not yet taken), maybe new requests, maybe a
    // bridge word.
    uint64_t end = cycle + clocks, next_br = cycle + 100, reset_left = 0, br_since_reset = 0;
    traffic = true;
    while (cycle < end) {
        top.reinit = 0;
        if (chance(1) && chance(50)) {                      // about every 20,000 clocks
            top.reinit = 1;
            if (ib.valid || ib.busy || db.valid || db.busy || !pending_writes.empty())
                cov["reinit during traffic"]++;
        }
        if (reset_left) {
            if (--reset_left == 0) top.cpu_reset = 0;
        } else if (chance(1) && chance(30)) {              // about every 33,000 clocks
            if (ib.busy) cov["reset during an iBus line"]++;
            if (db.busy) cov["reset during a dBus read"]++;
            top.cpu_reset = 1;
            reset_left = 1 + below(60);
            ib = IBus(); db = DBus();
            forget_pending(true);
            br_since_reset = 0;
            cov["CPU reset"]++;
        }
        if (!top.cpu_reset) {
            if (!ib.valid && !ib.busy && cycle >= ib.idle_until && chance(300)) ib_new();
            if (!db.valid && !db.busy && cycle >= db.idle_until && chance(500)) db_new();
        }
        top.br_wr = 0;
        if (cycle >= next_br) {
            uint32_t line = br_pool[below(br_pool.size())], w = line + below(8), v = rnd();
            top.br_wr = 1; top.br_addr = w; top.br_data = v;
            br_shadow[w] = v;
            next_br = cycle + 88 + below(200);
            br_since_reset++;
            cov["bridge word"]++;
        }
        drive();
        edge();
    }

    // quiet down, check the bridge counters, read the bridge region back over the dBus
    top.br_wr = 0; top.reinit = 0; ib.valid = false; db.valid = false; drive();
    for (int i = 0; i < 200 || ib.busy || db.busy; i++) { edge(); drive(); }
    if (top.br_overflow) ERR("bridge overflow at 88+ clocks per word");
    if (top.br_words != br_since_reset)
        ERR("bridge words %u, expected %llu", top.br_words, (unsigned long long)br_since_reset);
    for (auto &e : br_shadow) {
        db.wr = false; db.line = false; db.word = e.first; db.mask = 0xF; db.valid = true;
        shadow[e.first] = e.second;
        drive();
        while (db.valid || db.busy) { edge(); drive(); }
    }

    // a burst faster than the controller drains: overflow; Reset Enter clears it
    for (int i = 0; i < 64; i++) {
        top.br_wr = 1; top.br_addr = br_pool[0] + (i & 7); top.br_data = rnd(); drive(); edge();
        top.br_wr = 0; drive(); edge();
    }
    if (!top.br_overflow) ERR("no bridge overflow at 2 clocks per word"); else cov["bridge overflow"]++;
    top.cpu_reset = 1; drive(); edge(); edge();
    top.cpu_reset = 0; drive(); edge();
    if (top.br_overflow || top.br_words) ERR("Reset Enter did not clear the bridge counters");
    for (int i = 0; i < 100; i++) { edge(); drive(); }

    // The coverage: every case in need[] must have happened. Then the model's figures.
    static const char *const need[] = {
        "iBus line", "dBus line", "dBus single", "iBus and dBus in the same clock",
        "line read right after a write to it",
        "write mask 1", "write mask 2", "write mask 4", "write mask 8", "write mask 3", "write mask C",
        "write mask F",
        "CPU reset", "reset during an iBus line", "reset during a dBus read", "reset with a write queued",
        "bridge word", "bridge overflow", "precharge between jobs", "refresh during traffic",
        "reinit during traffic", "mode registers written again",
    };
    int missing = 0;
    for (const char *k : need) {
        printf("  %-38s %llu\n", k, (unsigned long long)cov[k]);
        if (!cov[k]) { printf("FAIL: case never happened: %s\n", k); missing++; }
    }
    printf("model errors %u, longest refresh gap %u clocks\n", top.model_errors, top.max_refresh);
    if (top.model_errors) ERR("the SDRAM model reported errors");
    if (top.max_refresh > 580) ERR("refresh gap above 580 clocks");
    if (errors || missing) { printf("FAIL: %d errors\n", errors); return 1; }
    printf("PASS: seed %u\n", seed);
    return 0;
}
