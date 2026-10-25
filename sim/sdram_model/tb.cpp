// sdram_model self-test: drives sim/models/sdram_model.v cycle by cycle, through the
// sdram_model_selftest wrapper that holds one model per parameter set (sel). Each case gets a
// fresh model, captures what it prints and compares the error tags it reported with the
// expected ones; read data is checked at the exact edges where it must be valid, and where a
// beat must not be driven, the poison on dq_o is checked too.
//
// Usage: Vsdram_model_selftest [-v]   (tools/sim.sh sdram_model [-v])
//   -v prints everything the model printed in each case, not only for failing ones.
//
// Cases: a correct controller in normal operation (CL2/BL2 single-location writes, CL2
// interleaved BL4, CL3/BL8 burst writes, interleaved BL8, full page, auto precharge, refresh at
// the limit) and the datasheet's burst interrupts with truncation and read masks allowed, both
// without any error; then one wrong sequence per check, mostly with its correct-by-one-clock
// twin; then one data check per fault-injection parameter.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include "Vsdram_model_selftest.h"
#include "verilated.h"

namespace {

// the wrapper's parameter sets (sel), see sdram_model_selftest in sdram_model.v
enum { C_STD, C_10NS, C_LONG, C_LAX, C_STUCK0, C_STUCK1, C_SHORT, C_LDQM, C_UDQM, C_SWAP, C_BA1, C_ROW, C_DROP };
// {ras_n, cas_n, we_n}
enum { LMR = 0, REF = 1, PRE = 2, ACT = 3, WR = 4, RD = 5, BST = 6, NOP = 7 };

// the same conversion as the model: up for minimum times, down for maximum times
int ns_up(double ns, double clk) { int n = int(ns / clk); if (n * clk < ns) n++; return n; }
int ns_down(double ns, double clk) { return int(ns / clk); }

struct Timing {
    int rcd, rp, ras, ras_max, rc, rrd, wr, rfc, mrd, init, refi;
    explicit Timing(int sel) {
        double clk = sel == C_10NS ? 10.0 : 13.468;
        rcd = ns_up(18, clk);
        rp = ns_up(18, clk);
        ras = ns_up(48, clk);
        ras_max = ns_down(100000, clk);
        rc = ns_up(sel == C_LONG ? 100 : 60, clk);
        rrd = ns_up(12, clk);
        wr = ns_up(15, clk);
        rfc = ns_up(80, clk);
        mrd = 2;
        init = ns_up(200000, clk);
        refi = ns_down(7812.5 * (sel == C_LONG ? 16.0 : 1.0), clk);
    }
};

uint32_t key(int b, int row, int col) { return uint32_t(b) << 23 | uint32_t(row) << 10 | uint32_t(col); }
// what a word never written reads as
uint16_t poison(uint32_t k) { return uint16_t(0xA5A5 ^ (k & 0xFFFF) ^ ((k >> 16 & 0x1FF) << 7)); }
// column of beat k of a burst from col (bl 0 = full page), p.7-8 Table 2
int order(int col, int k, int bl, int il) {
    if (bl == 0) return (col + k) & 1023;
    int base = col & ~(bl - 1), off = col & (bl - 1);
    return base | (il ? (off ^ k) : ((off + k) & (bl - 1)));
}

struct Dut {
    Vsdram_model_selftest *m;
    Timing t;
    int cyc = 0, fails = 0, refs = 0, last_ref = -1;
    int cl = 2, bl = 1, il = 0, wb_single = 0;      // as last loaded into the mode register
    int row[4] = {0, 0, 0, 0};                      // the row opened last per bank
    int dqm_default = 3;
    std::map<int, int> dqm_at;                      // edge -> DQM
    std::map<int, uint16_t> dq_at;                  // edge -> data the controller drives
    std::map<int, uint16_t> drove, off_seen;        // edge -> dq_o just before it (driving / not)
    std::map<int, uint16_t> want, want_off;         // edge -> expected read data / poison
    std::map<uint32_t, uint16_t> ref;               // what the memory should hold

    explicit Dut(int sel) : m(new Vsdram_model_selftest), t(sel) {
        m->sel = sel;
        m->clk = 0;
        m->cke = 0;
        m->ras_n = m->cas_n = m->we_n = 1;
        m->ba = 0;
        m->a = 0;
        m->dqm = 3;
        m->dq_i = 0;
        m->dq_i_en = 0;
        m->eval();
    }
    ~Dut() { m->final(); delete m; }

    void fail(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        printf("    FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
        fails++;
    }

    // one clock: the inputs for edge cyc, what the chip drove in the clock ending at it, the edge
    void tick(int cmd = NOP, int ba = 0, int a = 0, int cke = 1) {
        m->cke = cke;
        m->ras_n = cmd >> 2 & 1;
        m->cas_n = cmd >> 1 & 1;
        m->we_n = cmd & 1;
        m->ba = ba;
        m->a = a;
        auto q = dqm_at.find(cyc);
        m->dqm = q != dqm_at.end() ? q->second : dqm_default;
        auto d = dq_at.find(cyc);
        m->dq_i = d != dq_at.end() ? d->second : 0;
        m->dq_i_en = d != dq_at.end();
        if (m->dq_o_en) drove[cyc] = m->dq_o;
        else if (want_off.count(cyc)) off_seen[cyc] = m->dq_o;
        m->clk = 1;
        m->eval();
        m->clk = 0;
        m->eval();
        if (cmd == REF) { refs++; last_ref = cyc; }
        cyc++;
    }

    void nop(int n = 1) { for (int i = 0; i < n; i++) tick(); }
    void act(int b, int r) { row[b] = r; tick(ACT, b, r); }
    void pre(int b) { tick(PRE, b, 0); }
    void pre_all() { tick(PRE, 0, 1 << 10); }
    void refresh() { tick(REF); }
    void bst() { tick(BST); }
    void lmr(int op) {
        cl = op >> 4 & 7;
        bl = (op & 7) == 7 ? 0 : 1 << (op & 7);
        il = op >> 3 & 1;
        wb_single = op >> 9 & 1;
        tick(LMR, 0, op);
    }
    void emrs(int op) { tick(LMR, 2, op); }

    // the power-up sequence (p.5-6): 200 us of NOPs with DQM high, PRECHARGE ALL, AUTO
    // REFRESH x2, LOAD MODE REGISTER, LOAD EXTENDED MODE REGISTER, each after its wait
    void init(int mr = 0x221, int nref = 2, bool do_mr = true, bool do_emr = true, int wait = -1) {
        nop(wait < 0 ? t.init : wait);
        pre_all();
        nop(t.rp - 1);
        for (int i = 0; i < nref; i++) { refresh(); nop(t.rfc - 1); }
        if (do_mr) { lmr(mr); nop(t.mrd - 1); }
        if (do_emr) { emrs(0); nop(t.mrd - 1); }
        dqm_default = nref >= 2 && do_mr && do_emr ? 0 : 3;
    }
    // after a READ's edge: until the bus is free again (the chip holds it 2 clocks after the
    // last beat), so that the next command may be a WRITE
    void rw_gap() { nop(cl + bl + 1); }
    // all bursts over, every limit met, all banks precharged, one AUTO REFRESH
    void refresh_cycle() { nop(12); pre_all(); nop(t.rp - 1); refresh(); nop(t.rfc - 1); }

    uint16_t mem(int b, int r, int c) {
        auto it = ref.find(key(b, r, c));
        return it != ref.end() ? it->second : poison(key(b, r, c));
    }
    // WRITE at this edge, data[k] and masks[k] on edge +k; the first 'stored' beats reach the
    // memory (default: the whole burst, one beat in single-location mode)
    void write(int b, int col, std::vector<uint16_t> data, std::vector<int> masks = {}, bool ap = false, int stored = -1) {
        for (size_t k = 0; k < data.size(); k++) {
            dq_at[cyc + int(k)] = data[k];
            dqm_at[cyc + int(k)] = masks.empty() ? 0 : masks[k];
        }
        int len = wb_single ? 1 : bl;
        int n = stored >= 0 ? stored : wb_single ? 1 : int(data.size());
        for (int k = 0; k < n; k++) {
            int c = order(col, k, len, il), msk = masks.empty() ? 0 : masks[k];
            uint16_t old = mem(b, row[b], c), v = data[k];
            ref[key(b, row[b], c)] = uint16_t((msk & 2 ? old & 0xFF00 : v & 0xFF00) | (msk & 1 ? old & 0xFF : v & 0xFF));
        }
        tick(WR, b, (ap ? 1 << 10 : 0) | col);
    }
    // READ at this edge: beats 0..delivered-1 are valid at edge +cl+k, masked per byte lane by
    // the DQM two edges earlier (a masked lane carries the inverted data, a fully masked beat
    // is not driven); beats delivered..lost_to-1 are lost: not driven, dq_o = inverted data
    void read(int b, int col, int delivered = -1, bool ap = false, int lost_to = -1) {
        int r = cyc, n = delivered >= 0 ? delivered : bl;
        for (int k = 0; k < std::max(n, lost_to); k++) {
            int e = r + cl + k;
            uint16_t v = mem(b, row[b], order(col, k, bl, il));
            auto q = dqm_at.find(e - 2);
            int msk = k >= n ? 3 : q != dqm_at.end() ? q->second : dqm_default;
            want.erase(e);
            want_off.erase(e);
            if (msk == 3) want_off[e] = uint16_t(~v);
            else want[e] = uint16_t((msk & 2 ? ~v & 0xFF00 : v & 0xFF00) | (msk & 1 ? ~v & 0xFF : v & 0xFF));
        }
        tick(RD, b, (ap ? 1 << 10 : 0) | col);
    }
    // READ at this edge with explicit expected beats (for the fault cases)
    void read_expect(int b, int col, std::vector<uint16_t> vals) {
        for (size_t k = 0; k < vals.size(); k++) {
            want[cyc + cl + int(k)] = vals[k];
            want_off.erase(cyc + cl + int(k));
        }
        tick(RD, b, col);
    }

    void check() {
        for (auto &w : want) {
            auto it = drove.find(w.first);
            if (it == drove.end()) fail("edge %d: expected read data %04x, the chip did not drive", w.first, w.second);
            else if (it->second != w.second) fail("edge %d: read data %04x, expected %04x", w.first, it->second, w.second);
        }
        for (auto &d : drove)
            if (!want.count(d.first)) fail("edge %d: the chip drove %04x, no read data expected", d.first, d.second);
        for (auto &w : want_off) {
            if (drove.count(w.first)) continue;
            auto it = off_seen.find(w.first);
            if (it == off_seen.end()) fail("edge %d: never reached", w.first);
            else if (it->second != w.second)
                fail("edge %d: dq_o = %04x while not driving, expected the poison %04x", w.first, it->second, w.second);
        }
    }
};

// captures what the model prints (stdout, file descriptor 1) while a case runs
struct Capture {
    int saved = -1;
    FILE *tmp = nullptr;
    void start() {
        fflush(stdout);
        tmp = tmpfile();
        saved = dup(1);
        dup2(fileno(tmp), 1);
    }
    std::string stop() {
        fflush(stdout);
        dup2(saved, 1);
        close(saved);
        std::string s;
        rewind(tmp);
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, tmp)) > 0) s.append(buf, n);
        fclose(tmp);
        return s;
    }
};

std::vector<std::string> tags_of(const std::string &out) {
    std::vector<std::string> v;
    for (size_t p = out.find("ERROR ["); p != std::string::npos; p = out.find("ERROR [", p + 1)) {
        size_t q = out.find(']', p);
        v.push_back(out.substr(p + 7, q - p - 7));
    }
    std::sort(v.begin(), v.end());
    return v;
}

std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (auto &x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

int n_cases = 0, n_failed = 0;
bool verbose = false;                               // -v: print what the model printed in every case

void run(const char *name, int sel, std::vector<std::string> expect, const std::function<void(Dut &)> &body,
         const std::vector<std::string> &must_print = {}) {
    Capture cap;
    cap.start();
    uint32_t count;
    bool err;
    int fails;
    {
        Dut d(sel);
        body(d);
        d.check();
        count = d.m->error_count;
        err = d.m->error;
        fails = d.fails;
    }
    std::string out = cap.stop();
    std::vector<std::string> got = tags_of(out);
    std::sort(expect.begin(), expect.end());
    bool ok = got == expect && count == expect.size() && err == !expect.empty() && fails == 0;
    std::string missing;
    for (auto &s : must_print)
        if (out.find(s) == std::string::npos) { ok = false; missing += "\n    not printed: " + s; }
    n_cases++;
    n_failed += !ok;
    printf("%s  %-74s {%s}\n", ok ? "PASS" : "FAIL", name, join(got).c_str());
    if (!ok) printf("    expected {%s}, error_count %u, error %d%s\n", join(expect).c_str(), count, err, missing.c_str());
    if (!ok || verbose) {
        size_t p = 0, q;
        while ((q = out.find('\n', p)) != std::string::npos) { printf("    | %s\n", out.substr(p, q - p).c_str()); p = q + 1; }
    }
}

std::vector<uint16_t> seq(uint16_t first, int n) {
    std::vector<uint16_t> v;
    for (int i = 0; i < n; i++) v.push_back(uint16_t(first + i));
    return v;
}

// ---- a correct controller: no errors, every read beat right at its edge ----
void normal_operation(Dut &d) {
    // CL2, BL2, single-location writes
    d.init(0x221);
    if (!d.m->init_done) d.fail("init_done is low after the initialisation");
    d.act(0, 5);
    d.nop(d.t.rcd - 1);
    d.write(0, 10, {0x1234});
    d.write(0, 11, {0xABCD}, {1});                  // DQML high: only the high byte is written
    d.write(0, 12, {0x5555});
    d.dq_at[d.cyc] = 0xDEAD;                        // single-location mode ignores this
    d.nop();
    d.read(0, 10);                                  // back to back: 10 11, 11 10, 12 13 (never written)
    d.nop();
    d.read(0, 11);
    d.nop();
    d.read(0, 12);
    d.rw_gap();
    d.act(1, 0x1FFF);                               // second bank, last row, last column
    d.nop(d.t.rcd - 1);
    d.write(1, 1023, {0xBEEF});
    d.read(1, 1023);                                // WRITE -> READ on the next edge: 1023 1022
    d.rw_gap();
    d.write(0, 20, {0x0F0F});
    d.read(0, 20);
    d.rw_gap();                                     // READ -> WRITE once the bus is free
    d.write(0, 21, {0xF0F0});
    d.nop(d.t.wr - 1);
    d.pre(0);                                       // tWR after the data
    d.nop(d.t.rp - 1);
    d.act(0, 6);                                    // another row: nothing written there
    d.nop(d.t.rcd - 1);
    d.read(0, 10);
    d.rw_gap();
    d.write(0, 10, {0x6666});
    d.nop(d.t.wr - 1);
    d.pre(0);
    d.nop(d.t.rp - 1);
    d.act(0, 5);                                    // row 5 kept its data
    d.nop(d.t.rcd - 1);
    d.read(0, 10, -1, true);                        // READ with auto precharge: precharge at +BL = tRAS
    d.nop(1 + d.t.rp);
    d.act(0, 5);
    d.nop(d.t.rcd - 1);
    d.read(0, 12);
    d.rw_gap();
    d.act(2, 9);
    d.nop(d.t.rcd - 1);
    d.write(2, 0, {0x2222}, {}, true);              // WRITE with auto precharge: precharge tWR later
    d.nop(d.t.wr + d.t.rp - 1);
    d.act(2, 9);
    d.nop(d.t.rcd - 1);
    d.read(2, 0);
    d.rw_gap();

    // CL2, interleaved, BL4
    d.refresh_cycle();
    d.lmr(0x22A);
    d.nop(d.t.mrd - 1);
    d.act(3, 0x100);
    d.nop(d.t.rcd - 1);
    for (int i = 0; i < 4; i++) d.write(3, 0x40 + i, {uint16_t(0x4000 + i)});
    d.read(3, 0x41);                                // 41 40 43 42
    d.rw_gap();
    d.read(3, 0x42);                                // 42 43 40 41
    d.rw_gap();

    // AUTO REFRESH exactly every 7.8125 us
    d.refresh_cycle();
    for (int i = 0; i < 6; i++) {
        d.nop(d.t.refi - (d.cyc - d.last_ref));
        d.refresh();
    }
    d.nop(d.t.rfc - 1);

    // CL3, sequential, BL8, burst writes
    d.lmr(0x033);
    d.nop(d.t.mrd - 1);
    d.act(3, 0xABC);
    d.nop(d.t.rcd - 1);
    d.write(3, 0x20, seq(0xC000, 8), {0, 0, 2, 0, 0, 3, 0, 0});    // beat 2: DQMH, beat 5: both
    d.nop(7);
    d.read(3, 0x23);                                // 23..27 20 21 22
    d.nop(7);
    d.read(3, 0x20);                                // the next burst seamlessly, BL clocks later
    d.rw_gap();
    d.write(3, 0x30, seq(0xD000, 8));
    d.nop(7);
    d.read(3, 0x30);                                // WRITE -> READ right after the last data
    d.rw_gap();
    d.act(1, 0x55);
    d.nop(d.t.rcd - 1);
    d.read(1, 0x000, -1, true);                     // READ with auto precharge: precharge at +8
    d.rw_gap();
    d.act(1, 0x56);
    d.nop(d.t.rcd - 1);
    d.write(1, 0x100, seq(0xE000, 8), {}, true);    // WRITE with auto precharge: tWR after beat 7
    d.nop(7 + d.t.wr + d.t.rp - 1);
    d.act(1, 0x56);
    d.nop(d.t.rcd - 1);
    d.read(1, 0x100);
    d.rw_gap();

    // CL3, interleaved, BL8
    d.refresh_cycle();
    d.lmr(0x03B);
    d.nop(d.t.mrd - 1);
    d.act(3, 0xABC);
    d.nop(d.t.rcd - 1);
    d.read(3, 0x25);                                // 25 24 27 26 21 20 23 22
    d.rw_gap();

    // CL3, full page, burst writes: bursts end with BURST TERMINATE
    d.refresh_cycle();
    d.lmr(0x037);
    d.nop(d.t.mrd - 1);
    d.act(3, 0xABC);
    d.nop(d.t.rcd - 1);
    d.write(3, 1020, seq(0xF000, 6), {}, false, 6); // 1020..1023, 0, 1
    d.nop(5);
    d.bst();
    d.read(3, 1021, 7);                             // 1021..1023, 0..3: the edge of BST + CL-1 is the last
    d.nop(6);
    d.bst();
    d.nop(12);

    d.refresh_cycle();
    if (!d.m->init_done) d.fail("init_done is low at the end");
    if (d.m->refresh_count != uint32_t(d.refs)) d.fail("refresh_count %u, expected %d", d.m->refresh_count, d.refs);
    if (d.m->max_refresh_interval != uint32_t(d.t.refi))
        d.fail("max_refresh_interval %u, expected %d", d.m->max_refresh_interval, d.t.refi);
}

// ---- the datasheet's burst interrupts, with truncation and read masks allowed (C_LAX) ----
void datasheet_features(Dut &d) {
    d.init(0x221);                                  // CL2, BL2, single-location writes
    d.act(0, 5);
    d.nop(d.t.rcd - 1);
    d.write(0, 10, {0x1111});
    d.write(0, 11, {0x2222});
    d.write(0, 12, {0x3333});
    d.write(0, 13, {0x4444});
    d.read(0, 10, 1);                               // cut by the READ one clock later (p.18-21)
    d.read(0, 12);
    d.rw_gap();
    d.dqm_at[d.cyc] = 1;                            // read DQM, latency 2: DQML masks DQ7:0 of beat 0,
    d.dqm_at[d.cyc + 1] = 3;                        // both DQM mask beat 1 (not driven)
    d.read(0, 10);
    d.rw_gap();
    d.read(0, 12, 1, false, 2);                     // PRECHARGE one clock later: CL-1 = 1 more beat (p.24)
    d.pre(0);
    d.nop(d.t.rp - 1);

    d.refresh_cycle();                              // CL3, BL8, burst writes
    d.lmr(0x033);
    d.nop(d.t.mrd - 1);
    d.act(3, 0xABC);
    d.nop(d.t.rcd - 1);
    d.write(3, 0x20, seq(0x2000, 8));
    d.nop(7);
    d.write(3, 0x28, seq(0x2800, 8));
    d.nop(7);
    d.read(3, 0x20, 2);                             // cut by a READ two clocks later
    d.nop();
    d.read(3, 0x28);
    d.rw_gap();
    d.read(3, 0x20, 3, false, 8);                   // BURST TERMINATE: CL-1 = 2 more beats (p.24-26)
    d.nop(2);
    d.bst();
    d.rw_gap();
    d.read(3, 0x28, 4, false, 8);                   // PRECHARGE: the same
    d.nop(3);
    d.pre(3);
    d.nop(d.t.rp - 1);
    d.act(3, 0xABC);
    d.nop(d.t.rcd - 1 + 4);
    d.write(3, 0x30, {0x3000, 0x3001, 0x3002}, {}, false, 3);   // cut by a WRITE after 3 beats (p.30-31)
    d.nop(2);
    d.write(3, 0x38, seq(0x3800, 8));
    d.nop(7);
    d.write(3, 0x40, seq(0x4000, 4), {}, false, 4); // cut by a READ: data on its edge ignored (p.32, note 79)
    d.nop(3);
    d.dq_at[d.cyc] = 0xDEAD;
    d.read(3, 0x40);
    d.rw_gap();
    d.write(3, 0x48, seq(0x4800, 6), {}, false, 5); // cut by BURST TERMINATE: data on its edge ignored (p.33)
    d.nop(4);
    d.bst();
    d.read(3, 0x48);
    d.rw_gap();
    d.write(3, 0x50, seq(0x5000, 6), {0, 0, 0, 0, 3, 3});      // cut by PRECHARGE: tWR, DQM before and at it (p.31)
    d.nop(4);
    d.pre(3);
    d.nop(d.t.rp - 1);
    d.act(3, 0xABC);
    d.nop(d.t.rcd - 1);
    d.read(3, 0x50);
    d.rw_gap();
    d.act(1, 0x10);                                 // concurrent auto precharge (p.38-40)
    d.act(2, 0x20);
    d.nop(2);
    d.read(1, 0, 2, true);                          // READ with auto precharge in bank 1, cut by a
    d.nop();                                        // READ to bank 2: bank 1 precharges from there
    d.read(2, 0);
    d.nop(d.t.rp - 1);
    d.act(1, 0x11);
    d.rw_gap();
    d.write(1, 0x200, seq(0x7000, 3), {}, true, 3); // WRITE with auto precharge in bank 1, cut by a
    d.nop(2);                                       // WRITE to bank 2: precharge tWR after that
    d.write(2, 0x200, seq(0x7200, 8));
    d.nop(7);
    d.act(1, 0x11);
    d.nop(d.t.rcd - 1);
    d.read(1, 0x200);
    d.rw_gap();
    d.read(2, 0x200);
    d.rw_gap();
}

// a READ cut by a WRITE, the datasheet way (Figure 8: DQM high two clocks before the WRITE)
void read_cut_by_write(Dut &d) {
    d.init(0x033);
    d.act(3, 1);
    d.nop(d.t.rcd - 1);
    d.write(3, 0x10, seq(0x1000, 8));
    d.nop(7);
    for (int i = 1; i <= 4; i++) d.dqm_at[d.cyc + i] = 3;
    d.read(3, 0x10, 3, false, 8);                   // beats 0..2 masked, 3..7 lost to the WRITE
    d.nop(4);
    d.write(3, 0x40, seq(0x4000, 8));
    d.nop(7);
    d.read(3, 0x40);
    d.rw_gap();
}

void open0(Dut &d, int mr = 0x221) {                // initialised, bank 0 row 1 open, tRCD met
    d.init(mr);
    d.act(0, 1);
    d.nop(d.t.rcd - 1);
}

} // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    for (int i = 1; i < argc; i++) verbose |= std::string(argv[i]) == "-v";
    printf("sdram_model self-test (one fresh model per case; {} = the error tags it reported)\n");

    run("normal operation: CL2/BL2 single writes, CL2 interleaved BL4, CL3 BL8 burst writes, interleaved, full page, auto precharge, refresh at the limit",
        C_STD, {}, normal_operation,
        {"LOAD MODE REGISTER A = 0x0221: CAS latency 2, burst length 2, sequential, single-location writes (A9 = 1)",
         "LOAD MODE REGISTER A = 0x0033: CAS latency 3, burst length 8, sequential, burst writes (A9 = 0)",
         "LOAD MODE REGISTER A = 0x0037: CAS latency 3, burst length full page, sequential, burst writes (A9 = 0)",
         "LOAD EXTENDED MODE REGISTER A = 0x0000: partial-array self refresh all banks, drive strength 100 %"});
    run("datasheet burst interrupts and read DQM (truncation and read masks allowed)", C_LAX, {}, datasheet_features);
    run("init_done rises with the last initialisation command", C_STD, {}, [](Dut &d) {
        d.init(0x221, 2, true, false);
        if (d.m->init_done) d.fail("init_done high before LOAD EXTENDED MODE REGISTER");
        d.emrs(0);
        if (!d.m->init_done) d.fail("init_done low after LOAD EXTENDED MODE REGISTER");
    });

    // initialisation
    run("init: PRECHARGE ALL one clock before 200 us", C_STD, {"init_wait"}, [](Dut &d) { d.init(0x221, 2, true, true, d.t.init - 1); });
    run("init: PRECHARGE ALL at 200 us", C_STD, {}, [](Dut &d) { d.init(0x221, 2, true, true, d.t.init); });
    run("init: ACTIVE without LOAD EXTENDED MODE REGISTER", C_STD, {"init_order"}, [](Dut &d) { d.init(0x221, 2, true, false); d.act(0, 1); });
    run("init: ACTIVE without LOAD MODE REGISTER", C_STD, {"init_order"}, [](Dut &d) { d.init(0x221, 2, false, true); d.act(0, 1); });
    run("init: ACTIVE after only one AUTO REFRESH", C_STD, {"init_order"}, [](Dut &d) { d.init(0x221, 1); d.act(0, 1); });
    run("init: ACTIVE after the whole sequence", C_STD, {}, [](Dut &d) { d.init(); d.act(0, 1); });
    run("init: illegal MR (CAS latency code 001), then ACTIVE", C_STD, {"mode_reg", "init_order"}, [](Dut &d) {
        d.init(0x211);
        d.dqm_default = 3;
        d.act(0, 1);
    });
    run("init: DQM low during the 200 us wait", C_STD, {"dqm_init"}, [](Dut &d) { d.dqm_at[100] = 0; d.init(); });
    run("CKE low for one clock after the initialisation", C_STD, {"cke"}, [](Dut &d) { d.init(); d.tick(NOP, 0, 0, 0); d.nop(); });

    // timing, each one clock short and then just met
    run("tRCD: READ one clock early", C_STD, {"tRCD"}, [](Dut &d) { d.init(); d.act(0, 1); d.nop(d.t.rcd - 2); d.read(0, 0); d.nop(4); });
    run("tRCD: READ on time", C_STD, {}, [](Dut &d) { d.init(); d.act(0, 1); d.nop(d.t.rcd - 1); d.read(0, 0); d.nop(4); });
    run("tRCD: WRITE one clock early", C_STD, {"tRCD"}, [](Dut &d) { d.init(); d.act(0, 1); d.nop(d.t.rcd - 2); d.write(0, 0, {1}); });
    run("tRAS: PRECHARGE one clock early", C_STD, {"tRAS"}, [](Dut &d) { d.init(); d.act(0, 1); d.nop(d.t.ras - 2); d.pre(0); d.nop(2); });
    run("tRAS: PRECHARGE on time", C_STD, {}, [](Dut &d) { d.init(); d.act(0, 1); d.nop(d.t.ras - 1); d.pre(0); d.nop(2); });
    run("tRAS: auto precharge of a BL1 READ issued at tRCD", C_STD, {"tRAS"}, [](Dut &d) {
        open0(d, 0x220);
        d.read(0, 0, -1, true);
        d.nop(6);
    });
    run("tRAS: the same READ one clock later", C_STD, {}, [](Dut &d) {
        open0(d, 0x220);
        d.nop();
        d.read(0, 0, -1, true);
        d.nop(6);
    });
    run("tRAS max: row open one clock too long (refresh limit x16)", C_LONG, {"tRAS_max"}, [](Dut &d) {
        d.init();
        d.act(0, 1);
        d.nop(d.t.ras_max);
        d.pre(0);
        d.nop(d.t.rp - 1);
        d.refresh();
        d.nop(4);
    });
    run("tRAS max: row open exactly 100 us", C_LONG, {}, [](Dut &d) {
        d.init();
        d.act(0, 1);
        d.nop(d.t.ras_max - 1);
        d.pre(0);
        d.nop(d.t.rp - 1);
        d.refresh();
        d.nop(4);
    });
    run("tRP: ACTIVE one clock early", C_STD, {"tRP"}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd);
        d.pre(0);
        d.nop(d.t.rp - 2);
        d.act(0, 2);
    });
    run("tRP: ACTIVE on time", C_STD, {}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd);
        d.pre(0);
        d.nop(d.t.rp - 1);
        d.act(0, 2);
    });
    run("tRC: ACTIVE one clock early (tRC raised to 100 ns)", C_LONG, {"tRC"}, [](Dut &d) {
        d.init();
        d.act(0, 1);
        d.nop(d.t.ras - 1);
        d.pre(0);
        d.nop(d.t.rc - d.t.ras - 2);
        d.act(0, 2);
    });
    run("tRC: ACTIVE on time (tRC raised to 100 ns)", C_LONG, {}, [](Dut &d) {
        d.init();
        d.act(0, 1);
        d.nop(d.t.ras - 1);
        d.pre(0);
        d.nop(d.t.rc - d.t.ras - 1);
        d.act(0, 2);
    });
    run("tRRD: ACTIVE to another bank one clock early (10 ns clock)", C_10NS, {"tRRD"}, [](Dut &d) {
        d.init(0x231);
        d.act(0, 1);
        d.nop(d.t.rrd - 2);
        d.act(1, 1);
    });
    run("tRRD: on time (10 ns clock)", C_10NS, {}, [](Dut &d) {
        d.init(0x231);
        d.act(0, 1);
        d.nop(d.t.rrd - 1);
        d.act(1, 1);
    });
    run("tWR: PRECHARGE one clock early", C_STD, {"tWR"}, [](Dut &d) {
        d.init();
        d.act(0, 1);
        d.nop(d.t.ras - 1);
        d.write(0, 0, {7});
        d.nop(d.t.wr - 2);
        d.pre(0);
    });
    run("tWR: PRECHARGE on time", C_STD, {}, [](Dut &d) {
        d.init();
        d.act(0, 1);
        d.nop(d.t.ras - 1);
        d.write(0, 0, {7});
        d.nop(d.t.wr - 1);
        d.pre(0);
    });
    run("tRFC: ACTIVE one clock early", C_STD, {"tRFC"}, [](Dut &d) { d.init(); d.refresh(); d.nop(d.t.rfc - 2); d.act(0, 1); });
    run("tRFC: ACTIVE on time", C_STD, {}, [](Dut &d) { d.init(); d.refresh(); d.nop(d.t.rfc - 1); d.act(0, 1); });
    run("tMRD: ACTIVE one clock early", C_STD, {"tMRD"}, [](Dut &d) { d.init(); d.lmr(0x221); d.nop(d.t.mrd - 2); d.act(0, 1); });
    run("tMRD: ACTIVE on time", C_STD, {}, [](Dut &d) { d.init(); d.lmr(0x221); d.nop(d.t.mrd - 1); d.act(0, 1); });

    // banks
    run("ACTIVE to a bank whose row is open", C_STD, {"bank_open"}, [](Dut &d) { d.init(); d.act(0, 1); d.nop(d.t.rc - 1); d.act(0, 2); });
    run("READ to a bank with no open row", C_STD, {"bank_closed"}, [](Dut &d) { d.init(); d.read(2, 0); d.nop(4); });
    run("WRITE to a bank with no open row", C_STD, {"bank_closed"}, [](Dut &d) { d.init(); d.write(2, 0, {1}); });
    run("AUTO REFRESH with a bank open", C_STD, {"not_idle"}, [](Dut &d) { d.init(); d.act(1, 1); d.nop(d.t.ras); d.refresh(); });
    run("AUTO REFRESH one clock before tRP", C_STD, {"tRP"}, [](Dut &d) {
        d.init();
        d.act(1, 1);
        d.nop(d.t.ras - 1);
        d.pre(1);
        d.nop(d.t.rp - 2);
        d.refresh();
    });
    run("AUTO REFRESH at tRP", C_STD, {}, [](Dut &d) {
        d.init();
        d.act(1, 1);
        d.nop(d.t.ras - 1);
        d.pre(1);
        d.nop(d.t.rp - 1);
        d.refresh();
    });
    run("LOAD MODE REGISTER with a bank open", C_STD, {"not_idle"}, [](Dut &d) { d.init(); d.act(1, 1); d.nop(2); d.lmr(0x221); });
    run("PRECHARGE ALL one clock after a PRECHARGE (bank still precharging)", C_STD, {"precharge_busy"}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd);
        d.pre(0);
        d.nop(d.t.rp - 2);
        d.pre_all();
    });
    run("PRECHARGE ALL tRP after a PRECHARGE", C_STD, {}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd);
        d.pre(0);
        d.nop(d.t.rp - 1);
        d.pre_all();
    });
    run("READ to a bank during its READ with auto precharge (truncation allowed)", C_LAX, {"auto_precharge"}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd);
        d.read(0, 0, 1, true);
        d.read(0, 4);
        d.rw_gap();
    });
    run("READ with auto precharge in full-page mode", C_STD, {"auto_precharge"}, [](Dut &d) {
        open0(d, 0x027);
        d.read(0, 0, 3, true);
        d.nop(2);
        d.bst();
        d.nop(6);
    });

    // refresh interval
    run("refresh interval one clock longer than 7.8125 us", C_STD, {"refresh_interval"}, [](Dut &d) {
        d.init();
        d.nop(d.t.refi - (d.cyc - d.last_ref));
        d.refresh();
        d.nop(d.t.refi);
        d.refresh();
        d.nop(4);
        if (d.m->max_refresh_interval != uint32_t(d.t.refi + 1))
            d.fail("max_refresh_interval %u, expected %d", d.m->max_refresh_interval, d.t.refi + 1);
    });
    run("refresh interval exactly 7.8125 us", C_STD, {}, [](Dut &d) {
        d.init();
        d.nop(d.t.refi - (d.cyc - d.last_ref));
        d.refresh();
        d.nop(d.t.refi - 1);
        d.refresh();
        d.nop(4);
        if (d.m->max_refresh_interval != uint32_t(d.t.refi))
            d.fail("max_refresh_interval %u, expected %d", d.m->max_refresh_interval, d.t.refi);
    });

    // the bus
    run("bus: WRITE 2 clocks after the edge of the last read beat", C_STD, {"bus_conflict"}, [](Dut &d) {
        open0(d);
        d.read(0, 0);
        d.nop(d.cl + d.bl - 1 + 1);
        d.write(0, 4, {9});
    });
    run("bus: WRITE 3 clocks after the edge of the last read beat", C_STD, {}, [](Dut &d) {
        open0(d);
        d.read(0, 0);
        d.nop(d.cl + d.bl - 1 + 2);
        d.write(0, 4, {9});
    });
    run("bus: controller drives during a read beat", C_STD, {"bus_conflict"}, [](Dut &d) {
        open0(d);
        d.read(0, 0);
        d.dq_at[d.cyc + 1] = 0;
        d.nop(6);
    });
    run("bus: controller drives in the clock after the READ edge", C_STD, {"bus_conflict"}, [](Dut &d) {
        open0(d);
        d.read(0, 0);
        d.dq_at[d.cyc] = 0;
        d.nop(6);
    });
    run("bus: controller drives in the clock ending at the READ edge", C_STD, {}, [](Dut &d) {
        open0(d);
        d.dq_at[d.cyc] = 0;
        d.read(0, 0);
        d.nop(6);
    });
    run("READ cut by a WRITE with DQM as in Figure 8 (truncation, read masks allowed)", C_LAX, {"bus_conflict"}, read_cut_by_write);
    run("READ cut by a WRITE with DQM as in Figure 8 (defaults)", C_STD,
        {"bus_conflict", "truncation", "read_mask", "read_mask", "read_mask"}, read_cut_by_write);

    // CAS latency and clock
    run("CL2 at a 10 ns clock", C_10NS, {"cl_clock"}, [](Dut &d) { d.init(0x221); });
    run("CL3 at a 10 ns clock", C_10NS, {}, [](Dut &d) { d.init(0x231); });

    // write data
    run("WRITE with dq_i_en low", C_STD, {"write_undriven"}, [](Dut &d) { open0(d); d.tick(WR, 0, 3); d.nop(); });
    run("masked WRITE with dq_i_en low", C_STD, {}, [](Dut &d) { open0(d); d.dqm_at[d.cyc] = 3; d.tick(WR, 0, 3); d.nop(); });

    // mode registers
    run("MR: reserved burst length code 100", C_STD, {"mode_reg"}, [](Dut &d) { d.init(0x224); });
    run("MR: full page with interleaved burst type", C_STD, {"mode_reg"}, [](Dut &d) { d.init(0x02F); });
    run("MR: operating mode A8:A7 = 01", C_STD, {"mode_reg"}, [](Dut &d) { d.init(0x0A1); });
    run("MR: LOAD MODE REGISTER with BA = 01", C_STD, {"mode_reg"}, [](Dut &d) { d.init(); d.tick(LMR, 1, 0x221); });
    run("EMR: reserved PASR code 011", C_STD, {"ext_mode_reg"}, [](Dut &d) { d.init(); d.emrs(0x003); });
    run("EMR: reserved drive strength code 101", C_STD, {"ext_mode_reg"}, [](Dut &d) { d.init(); d.emrs(0x0A0); });
    run("EMR: A10 set", C_STD, {"ext_mode_reg"}, [](Dut &d) { d.init(); d.emrs(0x400); });
    run("EMR: drive strength 75 %, PASR one sixteenth (legal)", C_STD, {}, [](Dut &d) { d.init(); d.emrs(0x086); },
        {"LOAD EXTENDED MODE REGISTER A = 0x0086: partial-array self refresh one sixteenth, drive strength 75 %"});

    // truncation, read masks, burst terminate
    run("truncation: PRECHARGE one clock after a BL2 READ", C_STD, {"truncation"}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd - 1);
        d.read(0, 0, 1, false, 2);
        d.pre(0);
        d.nop(4);
    });
    run("truncation: PRECHARGE two clocks after a BL2 READ is none", C_STD, {}, [](Dut &d) {
        open0(d);
        d.nop(d.t.ras - d.t.rcd - 1);
        d.read(0, 0);
        d.nop();
        d.pre(0);
        d.nop(4);
    });
    run("truncation: second READ one clock after a BL2 READ", C_STD, {"truncation"}, [](Dut &d) {
        open0(d);
        d.read(0, 0, 1);
        d.read(0, 4);
        d.nop(5);
    });
    run("truncation: second READ two clocks after a BL2 READ is none", C_STD, {}, [](Dut &d) {
        open0(d);
        d.read(0, 0);
        d.nop();
        d.read(0, 4);
        d.nop(5);
    });
    run("truncation: BURST TERMINATE in a CL3 BL8 READ", C_STD, {"truncation"}, [](Dut &d) {
        open0(d, 0x033);
        d.read(0, 0, 3, false, 8);
        d.nop(2);
        d.bst();
        d.nop(12);
    });
    run("truncation: WRITE cuts a BL8 WRITE burst", C_STD, {"truncation"}, [](Dut &d) {
        open0(d, 0x033);
        d.write(0, 0, {1, 2, 3}, {}, false, 3);
        d.nop(2);
        d.write(0, 8, seq(0x100, 8));
        d.nop(7);
    });
    run("truncation: READ with auto precharge cut by a READ to another bank", C_STD, {"truncation"}, [](Dut &d) {
        d.init();
        d.act(1, 1);
        d.act(2, 2);
        d.nop(2);
        d.read(1, 0, 1, true);
        d.read(2, 0);
        d.nop(5);
    });
    run("read DQM: UDQM high two clocks before a beat", C_STD, {"read_mask"}, [](Dut &d) {
        open0(d);
        d.dqm_at[d.cyc + 1] = 2;
        d.read(0, 0);
        d.nop(5);
    });
    run("read DQM: DQM high where no beat follows two clocks later", C_STD, {}, [](Dut &d) {
        open0(d);
        d.dqm_at[d.cyc] = 3;
        d.nop();
        d.read(0, 0);
        d.nop(5);
    });
    run("PRECHARGE cuts a WRITE burst with DQM low on its edge (truncation allowed)", C_LAX, {"precharge_truncate"}, [](Dut &d) {
        open0(d, 0x033);
        d.nop(d.t.ras - d.t.rcd - 1);
        d.write(0, 0, seq(1, 6), {0, 0, 0, 0, 3, 0}, false, 5);
        d.nop(4);
        d.pre(0);
        d.nop(4);
    });
    run("PRECHARGE cuts a WRITE burst with DQM high on its edge (truncation allowed)", C_LAX, {}, [](Dut &d) {
        open0(d, 0x033);
        d.nop(d.t.ras - d.t.rcd - 1);
        d.write(0, 0, seq(1, 6), {0, 0, 0, 0, 3, 3}, false, 5);
        d.nop(4);
        d.pre(0);
        d.nop(4);
    });
    run("BURST TERMINATE with no burst in progress", C_STD, {"burst_terminate"}, [](Dut &d) { open0(d); d.bst(); });

    // fault injection: each parameter changes the data as intended, without protocol errors
    run("fault FAULT_DQ_STUCK0 = 0x0010: DQ4 reads and writes as 0", C_STUCK0, {}, [](Dut &d) {
        open0(d);
        d.write(0, 0, {0xFFFF});
        d.write(0, 1, {0x0010});
        d.read_expect(0, 0, {0xFFEF, 0x0000});
        d.rw_gap();
    });
    run("fault FAULT_DQ_STUCK1 = 0x0004: DQ2 reads and writes as 1", C_STUCK1, {}, [](Dut &d) {
        open0(d);
        d.write(0, 0, {0x0000});
        d.read_expect(0, 0, {0x0004, uint16_t(poison(key(0, 1, 1)) | 0x0004)});
        d.rw_gap();
    });
    run("fault FAULT_DQ_SHORT_A/B = 0/8: DQ0 and DQ8 wired-AND", C_SHORT, {}, [](Dut &d) {
        open0(d);
        d.write(0, 0, {0x0001});
        d.write(0, 1, {0x0101});
        d.read_expect(0, 0, {0x0000, 0x0101});
        d.rw_gap();
    });
    run("fault FAULT_LDQM_LOW: a write masked by DQML still writes DQ7:0", C_LDQM, {}, [](Dut &d) {
        open0(d);
        d.write(0, 0, {0x1234}, {1});
        d.read_expect(0, 0, {0x1234, poison(key(0, 1, 1))});
        d.rw_gap();
    });
    run("fault FAULT_UDQM_LOW: a write masked by DQMH still writes DQ15:8", C_UDQM, {}, [](Dut &d) {
        open0(d);
        d.write(0, 0, {0x1234}, {2});
        d.read_expect(0, 0, {0x1234, poison(key(0, 1, 1))});
        d.rw_gap();
    });
    run("fault FAULT_DQM_SWAP: DQML masks DQ15:8", C_SWAP, {}, [](Dut &d) {
        open0(d);
        d.write(0, 0, {0x1234}, {1});
        d.read_expect(0, 0, {uint16_t((poison(key(0, 1, 0)) & 0xFF00) | 0x34), poison(key(0, 1, 1))});
        d.rw_gap();
    });
    run("fault FAULT_BA1_STUCK0: bank 2 aliases bank 0", C_BA1, {}, [](Dut &d) {
        d.init();
        d.act(0, 7);
        d.act(2, 7);
        d.nop(d.t.rcd - 1);
        d.write(2, 5, {0xAAAA});
        d.write(0, 5, {0x5555});
        d.read_expect(2, 5, {0x5555, poison(key(0, 7, 4))});
        d.rw_gap();
    });
    run("fault FAULT_ROW_ALIAS_SRC/DST = 0/12: row 0x0001 aliases row 0x1001", C_ROW, {}, [](Dut &d) {
        d.init();
        d.act(0, 0x0001);
        d.nop(d.t.rcd - 1);
        d.write(0, 0, {0x1111});
        d.nop(d.t.wr - 1);
        d.pre(0);
        d.nop(d.t.rp - 1);
        d.act(0, 0x1001);
        d.nop(d.t.rcd - 1);
        d.write(0, 0, {0x2222});
        d.nop(d.t.wr - 1);
        d.pre(0);
        d.nop(d.t.rp - 1);
        d.act(0, 0x0001);
        d.nop(d.t.rcd - 1);
        d.read_expect(0, 0, {0x2222, poison(key(0, 0x1001, 1))});
        d.rw_gap();
    });
    run("fault FAULT_DROP_WRITE_N = 3: every third write is lost", C_DROP, {}, [](Dut &d) {
        open0(d);
        for (int c = 0; c < 6; c++) d.write(0, c, {uint16_t(0x100 + c)});
        d.read_expect(0, 0, {0x100, 0x101});
        d.nop();
        d.read_expect(0, 2, {poison(key(0, 1, 2)), 0x103});
        d.nop();
        d.read_expect(0, 4, {0x104, poison(key(0, 1, 5))});
        d.rw_gap();
    });

    printf("==== sdram_model self-test: %d cases, %d failed ====\n", n_cases, n_failed);
    printf(n_failed ? "FAIL\n" : "PASS\n");
    return n_failed ? 1 : 0;
}
