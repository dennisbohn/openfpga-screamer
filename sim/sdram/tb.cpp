// Random test of the SDRAM front end and controller (docs/speicher.md): sdram_port +
// sdram + the SDRAM model, driven by bus models that follow VexRiscv's cache-bus
// protocol, a bridge, random CPU resets and reinit pulses.
//
// Usage: Vsdram_system [seed, default 1] [clocks, default 2000000]
//
// Addresses come from a small pool (a few lines in a few rows of every bank, row and bank
// boundaries, the last line of the memory), so reads keep hitting words that writes just
// changed. Gaps are mostly 0..3 clocks; the dBus follows a write with a read of the same
// line now and then; iBus and dBus often handshake in the same clock; CPU resets of 1 to
// 60 clocks and reinit pulses (the short initialisation after a clock phase step) fall at
// random times. The bridge writes its own region at least 88 clocks apart; at the end a
// fast burst must set the overflow bit and Reset Enter must clear it.
//
// Checks: words come only for requests, in the right number, never in the handshake clock;
// data equals a shadow memory under the ordering rule (a dBus read sees every write taken
// before it; an iBus line may also see writes taken in its handshake clock or after); the
// READ and WRITE commands at the SDRAM pins follow the jobs in the order the controller
// took them, at the right columns and rows; cpu_writes_idle never reads 1 while a CPU
// write is not done; the model reports no error and no refresh gap above 580 clocks; every
// case in the coverage list happened at least once.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Vsdram_system.h"

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
#define ERR(...) do { if (errors++ < 30) { printf("clock %llu: ", (unsigned long long)cycle); printf(__VA_ARGS__); printf("\n"); } \
                      if (errors >= 1000) { printf("FAIL: 1000 errors, stopped\n"); exit(1); } } while (0)
// watchdog: a request without an answer, or a write that never reaches the pins, would
// hang the CPU; the longest legal pause is a refresh or a reinit, well below this
static const uint64_t STALL_LIMIT = 20000;
static uint64_t last_progress = 0;

// shadow of the CPU region: word index -> value; words not in it are unknown
static std::map<uint32_t, uint32_t> shadow;
struct WriteLog { uint64_t at; uint32_t word, value; };
static std::deque<WriteLog> wlog;           // CPU writes, for iBus reads that may see later ones

static std::vector<uint32_t> pool;          // line base word indices for CPU traffic
static std::vector<uint32_t> br_pool;       // line base word indices for the bridge
static std::map<uint32_t, uint32_t> br_shadow;

// coverage
static std::map<std::string, uint64_t> cov;

// expected SDRAM commands, from the jobs the controller took
struct Cmd { bool write; int bank; int row; int col; };
static std::deque<Cmd> expect_cmds;
static int open_row[4] = { -1, -1, -1, -1 };
static bool traffic = false;                // the random traffic has started
// CPU writes taken but not yet done at the pins: count of WRITE commands still to come
struct PendingWrite { uint32_t word; int halves_left; bool taken; };
static std::deque<PendingWrite> pending_writes;

static uint32_t lane_data(uint32_t mask) {
    uint32_t b = rnd() & 0xFF, h = rnd() & 0xFFFF;
    switch (mask) {
    case 0x1: case 0x2: case 0x4: case 0x8: return b * 0x01010101u;
    case 0x3: case 0xC: return h * 0x00010001u;
    default: return rnd();
    }
}
static const uint32_t masks[7] = { 0x1, 0x2, 0x4, 0x8, 0x3, 0xC, 0xF };
static uint32_t bytes_of(uint32_t mask) {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++) if (mask >> i & 1) m |= 0xFFu << (8 * i);
    return m;
}

// --- bus models ---
struct IBus {
    bool valid = false; uint32_t line = 0;      // request waiting for its handshake
    bool busy = false; uint64_t hs_at = 0;      // words to come
    int got = 0; uint32_t line_w = 0;
    uint32_t before[8]; bool known[8];
    uint64_t idle_until = 0;
} ib;

struct DBus {
    bool valid = false, wr = false, line = false; uint32_t word = 0, data = 0, mask = 0;
    bool busy = false; uint64_t hs_at = 0; int want = 0, got = 0; uint32_t word_hs = 0;
    uint32_t expect[8]; bool known[8];
    uint64_t idle_until = 0;
    uint32_t last_write_line = 0; uint64_t last_write_at = 0;
} db;

static void ib_new() {
    ib.line = pool[below(pool.size())];
    ib.valid = true;
}

static void db_new() {
    uint32_t r = below(100);
    uint32_t line = pool[below(pool.size())];
    if (r < 40) {                               // write
        db.wr = true; db.line = false;
        db.word = line + below(8);
        db.mask = masks[below(7)];
        db.data = lane_data(db.mask);
    } else if (r < 70 || (db.last_write_at && cycle - db.last_write_at < 4)) {
        db.wr = false; db.line = true;          // line read, often of the line just written
        db.word = (db.last_write_at && cycle - db.last_write_at < 4 && chance(700)) ? db.last_write_line : line;
        db.mask = masks[below(7)];              // the D$ sends the lanes of the missing load
    } else {
        db.wr = false; db.line = false;
        db.word = line + below(8);
        db.mask = masks[below(7)];
    }
    db.valid = true;
}

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

static void forget_pending(bool reset) {
    // a CPU reset may drop a write still in the port register: its word becomes unknown
    for (auto &p : pending_writes)
        if (!p.taken && reset) { shadow.erase(p.word); cov["reset with a write queued"]++; }
    std::deque<PendingWrite> keep;
    for (auto &p : pending_writes) if (p.taken) keep.push_back(p);
    pending_writes = keep;
}

static void check_ib_word(uint32_t data, bool error) {
    if (!ib.busy) { ERR("iBus word without a request"); return; }
    if (cycle <= ib.hs_at) ERR("iBus word in the handshake clock");
    if (error) ERR("iBus word with the error flag");
    int i = ib.got++;
    uint32_t w = ib.line_w + i;
    if (ib.known[i] && data != ib.before[i]) {
        bool later = false;
        for (auto &e : wlog) if (e.word == w && e.at >= ib.hs_at && e.value == data) later = true;
        if (!later) ERR("iBus word %u of line %06x: %08x, expected %08x", i, ib.line_w << 2, data, ib.before[i]);
        else cov["iBus saw a later write"]++;
    }
    if (ib.got == 8) { ib.busy = false; ib.idle_until = cycle + gap(); }
}

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

static void take_job() {
    // the controller takes a job at the coming edge: queue its commands
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

static void see_command() {
    int ras = top.dram_ras_n, cas = top.dram_cas_n, we = top.dram_we_n;
    int bank = top.dram_ba, a = top.dram_a;
    if (!ras && cas && we) { open_row[bank] = a; return; }                      // ACTIVATE
    if (!ras && cas && !we) {                                                   // PRECHARGE
        if (a & 0x400) for (int b = 0; b < 4; b++) open_row[b] = -1; else open_row[bank] = -1;
        if (!expect_cmds.empty()) cov["precharge between jobs"]++;
        return;
    }
    if (!ras && !cas && we) { if (!expect_cmds.empty() || ib.busy || db.busy) cov["refresh during traffic"]++; return; }
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
                if (it->taken && it->word == ((uint32_t(open_row[bank]) << 11) | (bank << 9) | ((a & 0x3FF) >> 1))) {
                    if (--it->halves_left == 0) pending_writes.erase(it);
                    break;
                }
        }
    }
}

static void edge() {
    // clock low: inputs for the coming edge are set; combinational outputs settle
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
            if (db.line && db.last_write_at && db.hs_at - db.last_write_at <= 4 && db.word_hs == db.last_write_line)
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

    // the pool: 3 rows of every bank, 3 lines each, plus row and bank boundaries and the end
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

    // write every pool word once, so reads can be checked
    for (uint32_t line : pool)
        for (int w = 0; w < 8; w++) {
            db.wr = true; db.line = false; db.word = line + w; db.mask = 0xF; db.data = rnd(); db.valid = true;
            drive();
            while (db.valid) { edge(); drive(); }
        }
    while (!pending_writes.empty()) { edge(); drive(); }

    // random traffic
    uint64_t end = cycle + clocks, next_br = cycle + 100, reset_left = 0, br_since_reset = 0;
    traffic = true;
    while (cycle < end) {
        top.reinit = 0;
        if (chance(1) && chance(50)) {                      // about every 20,000 clocks
            top.reinit = 1;
            if (ib.valid || ib.busy || db.valid || db.busy || !pending_writes.empty()) cov["reinit during traffic"]++;
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
    if (top.br_words != br_since_reset) ERR("bridge words %u, expected %llu", top.br_words, (unsigned long long)br_since_reset);
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

    // coverage and the model
    static const char *const need[] = {
        "iBus line", "dBus line", "dBus single", "iBus and dBus in the same clock", "line read right after a write to it",
        "write mask 1", "write mask 2", "write mask 4", "write mask 8", "write mask 3", "write mask C", "write mask F",
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
