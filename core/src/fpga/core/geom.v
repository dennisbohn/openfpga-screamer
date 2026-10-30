//
// Geometry unit: the pieces of the track (placed meshes) one after the other, bit for bit
// the C model firmware/geom.c (geom.h has the arithmetic). The CPU writes each piece's place
// in view space into a buffer of 2048 words (113 pieces) and goes on with its work; the
// unit takes a piece once all of it is in and reads the mesh from the SDRAM itself (block
// header, vertices, the face
// row for the eye's side, faces, texture coordinates, MLB materials), takes the vertices
// into view space (block RAM, up to 512), leaves out the faces turned away and hands each
// other face to the setup unit as a polygon in fixed point (routine 224 like 16, blended
// and texel 0 see-through: raster.v's texture size bits 30 and 31).
//
// One multiplier, and two more for the vertices' rows and the side test (operands registered,
// the product two clocks after it was issued), and a read cache of 4 KB (direct-mapped,
// lines of 32 bytes from the SDRAM). A step of the sequence below reads a value or works on
// what it read, or both: a read puts its address straight into the cache's RAMs, the next
// clock waits for them, and the step after takes the value when the line is there (a read
// takes two clocks when it hits); on a miss that step waits while the line comes in. The read
// addresses come from one adder (a base register plus an offset, the base often the address
// read before), the addresses made of a value just read (a base plus a word or half from the
// mesh) from a second one; the piece's matrix, origin and eye, and the corner and normal of
// the side test, wait in small block RAMs.
//
// Registers (soc.v maps them at 0xF000_3100 + 4 * wr_addr):
//   17      SDRAM byte address of the MLB      18  [8] the buffer drawn into
//   20      faces handed on since the last clear (write: clear)
//   21      clocks busy since the last clear (write: clear)
//   22      write: forget the cached lines (after the mesh data changed)
//   23      write: the next word of a piece into the buffer (soc.v holds the write back while
//           it is full). A piece is 18 words: local to view (rows r, u, f of columns x, y, z,
//           16 fraction bits, within +-2^17), its origin in view space and the eye in its
//           frame (8 fraction bits each), the SDRAM byte addresses of the mesh's object and
//           its block, and a word with [4] and [2:0] the face row for the eye's side (0..15,
//           read at block +0x28 + 4 x row; [4] is 8, the eye below the block's x-z plane, the
//           game's 0x8a598), [3] a moving part's block (the faces turned away are the others:
//           its frames mirror it, the game's 0x8acf0) and [29:16] the texture phase: of a
//           texture with n frames (texture record +0) frame n x phase >> 14 is drawn (the
//           game's 0xd5980; the frames' data offsets from record +8 on).
//

module geom (
    input   wire            clk,
    input   wire            reset,

    input   wire            wr,
    input   wire    [4:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            busy,
    output  wire            full,               // the piece buffer takes no word now
    output  reg     [31:0]  faces = 0,
    output  reg     [31:0]  busy_clocks = 0,

    // lines from the SDRAM (shared with the rasterizer's port, soc.v)
    output  reg             mem_valid = 0,
    input   wire            mem_ready,
    output  reg     [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data,

    // a polygon for the setup unit: taken in a clock with poly_valid and poly_ready
    output  reg             poly_valid = 0,
    input   wire            poly_ready,
    output  reg     [2:0]   poly_n,
    output  reg     [9:0]   poly_mode,          // [7:0] colour, [8] buffer, [9] textured
    output  reg     [31:0]  poly_tex,
    output  reg     [31:0]  poly_size,
    output  reg     [575:0] poly_c              // corner k at [144 k +: 144]: {t, s, f, u, r}
);

//
// registers the CPU writes, and the piece buffer
//
    reg  [25:0] mlb;
    reg         buffer;
    reg         inval_req = 1;
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] fifo [0:2047];
    reg  [11:0] f_wp = 0, f_rp = 0;             // a wrap bit over the address
    reg  [10:0] f_ra;
    reg  [31:0] f_q;
    wire [11:0] f_count = f_wp - f_rp;
    wire        push = wr && wr_addr == 5'd23 && !reset;
    assign full = f_count >= 12'd2046;          // room for the write soc.v may have under way

always @(posedge clk) begin
    if(wr && wr_addr == 5'd17) mlb <= wr_data[25:0];
    if(wr && wr_addr == 5'd18) buffer <= wr_data[8];
    if(push) fifo[f_wp[10:0]] <= wr_data;
    f_q <= fifo[f_ra];
end

//
// the sequence's registers
//
    reg  [5:0]  pc = 0, ret;
    reg  [4:0]  pk;                             // word of the piece being taken
    reg  [25:0] obj, blk, verts, uvs, ep, face, texdata;
    reg  [3:0]  row;
    reg         mirror;
    reg  [13:0] phase;
    reg  [29:0] fmul;                           // frames x phase
    reg  [9:0]  nvert;                          // 0..512 once step 7 let the piece through
    reg         nbad;                           // the block's vertex count is not 0..512
    reg  [9:0]  vi, nf;
    reg  signed [39:0] acc [0:2];               // the origin << 8 plus a row's products
    reg  [2:0]  ms;                             // product steps
    reg  [2:0]  nc, j;
    reg         textured, cut, blend, okv;
    reg  [8:0]  tw, th;
    reg  [7:0]  colour;

    localparam  [5:0]   P_IDLE = 6'd0, P_READ = 6'd1, P_DONE = 6'd2, P_TAKE = 6'd48;

//
// read cache: 128 lines of 32 bytes; a read gives the word holding byte address rd_addr
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] cdata [0:1023];
    reg  [14:0] ctag [0:127];                   // {valid, address[25:12]}
    reg  [9:0]  cd_ra;
    reg  [31:0] cd_q;
    reg  [6:0]  ct_ra, ct_wa;
    reg         ct_we;
    reg  [14:0] ct_wd, ct_q;

    localparam  [2:0]   C_IDLE = 3'd0, C_LOOK = 3'd1, C_MISS = 3'd3, C_FILL = 3'd4, C_INVAL = 3'd5,
                        C_SETTLE = 3'd6;
    reg  [2:0]  cstate = C_INVAL;
    reg  [25:0] rd_addr;                        // the value the step in pc waits for (rd_check)
    reg         rd_check = 0;
    reg  [2:0]  fill_cnt;
    reg  [6:0]  inval_cnt = 0;
    // the words of a line go into the RAM as they come in (the step waits until C_LOOK has
    // read the line again); the tag a clock after the last one
    wire        cd_we = cstate == C_FILL && mem_rsp_valid;
always @(posedge clk) begin
    if(cd_we) cdata[{rd_addr[11:5], fill_cnt}] <= mem_rsp_data;
    cd_q <= cdata[cd_ra];
    if(ct_we) ctag[ct_wa] <= ct_wd;
    ct_q <= ctag[ct_ra];
end
    wire        rd_hit = ct_q == {1'b1, rd_addr[25:12]};
    wire        hold = rd_check && !rd_hit;     // its line is not there yet: the step waits
    wire [31:0] rd_word = cd_q;

    // the value read: the word, a halfword of it signed or unsigned
    wire [15:0] rd_half = rd_addr[1] ? rd_word[31:16] : rd_word[15:0];
    wire signed [31:0] rd_s16 = {{16{rd_half[15]}}, rd_half};
    wire [31:0] rd_u16 = {16'd0, rd_half};

//
// vertex RAM: 512 vertices in view space, {f, u, r}, written from the accumulators in step 13
// (which waits for no read)
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [95:0] vram [0:511];
    reg  [8:0]  v_ra;
    reg  [95:0] v_q;
always @(posedge clk) begin
    if(pc == 6'd13) vram[vi[8:0]] <= {acc[2][39:8], acc[1][39:8], acc[0][39:8]};
    v_q <= vram[v_ra];
end

//
// the piece's matrix, origin and eye, and the corner and normal of the side test, in small
// RAMs: p0..p2 one for each row of the matrix (words 0..2 its columns x, y, z, [17:0] used,
// word 3 the origin's coordinate), words 4..6 of p0 the eye; vc a vertex or the face's first
// corner (x, y, z, as read), nm the face's normal. The piece goes in as it is taken (no read
// is waited for then). All are read with p_ra of the clock before: the vertex transform the
// origin (step 11 for the accumulators in 12's first clock), then the columns 0..2 for 12's
// next three clocks, the side test the eye, the corner and the normal (26 for its clocks
// 1..3).
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] p0 [0:7];
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] p1 [0:3];
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] p2 [0:3];
    (* ramstyle = "no_rw_check, M10K" *) reg  [15:0] vc [0:3];
    (* ramstyle = "no_rw_check, M10K" *) reg  [15:0] nm [0:3];
    reg  [31:0] pq0, pq1, pq2;
    reg  signed [15:0] vq, nq;
    reg  [2:0]  p_wa;
    reg  [2:0]  p_we;                           // which of p0, p1, p2 takes word pk - 1
    reg  [1:0]  vn_wa;
    wire        side = pc[4];                   // steps 25, 26 (11, 12: the vertex transform)
    wire [2:0]  p_ra = {side, pc == 6'd12 || pc == 6'd26 ? ms[1:0] : 2'd3};
    wire        v_we = !hold && (pc == 6'd9 || pc == 6'd10 || pc == 6'd11 ||
                                 pc == 6'd20 || pc == 6'd21 || pc == 6'd22);
    wire        n_we = !hold && (pc == 6'd23 || pc == 6'd24 || pc == 6'd25);
always @* begin
    p_we = 3'd0;
    p_wa = 3'd0;
    if(pc == P_TAKE) case(pk)
    5'd1:  begin p_we = 3'b001; p_wa = 3'd0; end
    5'd2:  begin p_we = 3'b001; p_wa = 3'd1; end
    5'd3:  begin p_we = 3'b001; p_wa = 3'd2; end
    5'd4:  begin p_we = 3'b010; p_wa = 3'd0; end
    5'd5:  begin p_we = 3'b010; p_wa = 3'd1; end
    5'd6:  begin p_we = 3'b010; p_wa = 3'd2; end
    5'd7:  begin p_we = 3'b100; p_wa = 3'd0; end
    5'd8:  begin p_we = 3'b100; p_wa = 3'd1; end
    5'd9:  begin p_we = 3'b100; p_wa = 3'd2; end
    5'd10: begin p_we = 3'b001; p_wa = 3'd3; end
    5'd11: begin p_we = 3'b010; p_wa = 3'd3; end
    5'd12: begin p_we = 3'b100; p_wa = 3'd3; end
    5'd13: begin p_we = 3'b001; p_wa = 3'd4; end
    5'd14: begin p_we = 3'b001; p_wa = 3'd5; end
    5'd15: begin p_we = 3'b001; p_wa = 3'd6; end
    default: ;
    endcase
    case(pc)
    6'd10, 6'd21, 6'd24: vn_wa = 2'd1;
    6'd11, 6'd22, 6'd25: vn_wa = 2'd2;
    default:             vn_wa = 2'd0;
    endcase
end
always @(posedge clk) begin
    if(p_we[0]) p0[p_wa] <= f_q;
    if(p_we[1]) p1[p_wa[1:0]] <= f_q;
    if(p_we[2]) p2[p_wa[1:0]] <= f_q;
    if(v_we) vc[vn_wa] <= rd_half;
    if(n_we) nm[vn_wa] <= rd_half;
    pq0 <= p0[p_ra];
    pq1 <= p1[p_ra[1:0]];
    pq2 <= p2[p_ra[1:0]];
    vq <= vc[p_ra[1:0]];
    nq <= nm[p_ra[1:0]];
end

//
// the multipliers
//
    // mul: a vertex's row r, the vertex number of an offset, the texture coordinates; mul1 and
    // mul2 the rows u and f, and in the side test b = (c0 << 8) - e (33 bits: an int16 << 8
    // less an int32) in two halves, b = b[32:17] 2^17 + b[16:0], times the normal. Every
    // product is below 2^33. (Steps 37 and 40 take a product while its operands still stand.)
    reg  signed [15:0] mul_a, mul1_a;
    reg  signed [17:0] mul_b, mul1_b, mul2_a, mul2_b;
    reg  signed [33:0] mul_p, mul1_p, mul2_p;
    wire signed [32:0] sb = {{9{vq[15]}}, vq, 8'd0} - {pq0[31], pq0};
always @(posedge clk) begin
    mul_p <= mul_a * mul_b;
    mul1_p <= mul1_a * mul1_b;
    mul2_p <= mul2_a * mul2_b;
    // (taken every clock: only the products of steps 12 and 26 are used, two clocks on)
    mul1_a <= side ? sb[32:17] : vq;
    mul1_b <= side ? {{2{nq[15]}}, nq} : pq1[17:0];
    mul2_a <= side ? {1'b0, sb[16:0]} : {{2{vq[15]}}, vq};
    mul2_b <= side ? {{2{nq[15]}}, nq} : pq2[17:0];
end

    // the side test's sum b . n = acc1 2^17 + acc2 (|acc1| < 2^32, |acc2| < 2^34)
    wire [49:0] dsum = {acc[1][32:0], 17'd0} + {{15{acc[2][34]}}, acc[2][34:0]};

    function [8:0] size9(input signed [31:0] v);        // 1..256
        size9 = v < 32'sd1 ? 9'd1 : v > 32'sd256 ? 9'd256 : v[8:0];
    endfunction

`ifdef GEOM_STATS
    integer     st_read = 0, st_miss = 0, st_setup = 0, st_pieces = 0, st_verts = 0, st_faces = 0, st_away = 0;
`endif

    assign busy = pc != P_IDLE || poly_valid || f_count != 0;

//
// the address adders. ra_sum: a base plus an offset (19 bits, two's complement), the next
// read's address; where a read follows one of the same record the base is the address read
// before (rd_addr). Every clock that waits for no line takes it into rd_addr and the cache's
// read address, also where nothing is read; where rd_addr must stand the offset is 0 (P_READ,
// while the RAMs read; 11..13 and 37, between a read and the next one from it). Steps without
// a read go on from it (7: the vertices less 2, 17: the next face, 45: a texture record's
// frames). rv_sum: a base plus the value just read, for the registers holding addresses in
// the mesh, and for rd_addr when the next read is at a corner (19), a material (28), a
// texture record (32) or a texture coordinate (36); the cache's read address never takes it.
//
    reg  [25:0] ra_base, rv_base, rv_val;
    reg  [18:0] ra_off;
always @* begin
    ra_base = rd_addr;
    ra_off = 19'd2;                             // 8, 9, 10, 18, 20, 21, 38: the next halfword
    case(pc)
    P_READ, 6'd11, 6'd12, 6'd13, 6'd15, 6'd37, 6'd47, 6'd49:
           ra_off = 19'd0;
    6'd3:  begin ra_base = blk; ra_off = 19'h10; end            // the vertex count
    6'd4:  ra_off = 19'hC;                                      // blk + 0x1C
    6'd5:  ra_off = 19'h8;                                      // blk + 0x24
    6'd6:  ra_off = {12'd0, {1'b0, row} + 5'd1, 2'd0};          // blk + 0x28 + 4 row
    6'd7:  begin ra_base = verts; ra_off = -19'sd2; end
    6'd14: begin ra_base = face; ra_off = 19'h6; end
    6'd16: begin ra_base = ep; ra_off = 19'd0; end
    6'd17: ra_base = ep;                                        // ep + 2
    6'd22: begin ra_base = face; ra_off = 19'h14; end
    6'd23, 6'd24, 6'd30: ra_off = 19'h4;                        // face + 0x18, 0x1C; mat + 0xC
    6'd27: begin ra_base = face; ra_off = 19'h4; end
    6'd29, 6'd45: ra_off = 19'h8;                               // mat + 8; texrec + 8
    6'd31: ra_off = -19'sd8;                                    // mat + 4
    6'd35: begin ra_base = face; ra_off = {15'd0, 1'b1, j[1:0], 1'b0}; end  // face + 8 + 2 j
    6'd46: ra_off = {1'b0, fmul[29:14], 2'd0};                  // texrec + 8 + 4 frame
    default: ;
    endcase
    case(pc)
    6'd5, 6'd6:  rv_base = obj;
    6'd7, 6'd17: rv_base = blk;
    6'd19:       rv_base = verts;
    6'd36:       rv_base = uvs;
    default:     rv_base = mlb;                 // 28, 32, 34
    endcase
    case(pc)
    6'd17, 6'd19, 6'd36: rv_val = {10'd0, rd_half};
    6'd28:               rv_val = {{10{rd_half[15]}}, rd_half};
    default:             rv_val = rd_word[25:0];
    endcase
end
    wire [25:0] ra_sum = ra_base + {{7{ra_off[18]}}, ra_off};
    wire [25:0] rv_sum = rv_base + rv_val;
    wire        untex = rd_word == 32'd128 || rd_word == 32'd192;    // 192 (0xcae84): one colour too
    // (step 29 of a face in one colour reads face + 2: the choice comes from the value just
    // read, so it picks between two sums, not the adder's inputs)
    wire [25:0] rd_a = pc == 6'd29 && untex ? face + 26'd2 : ra_sum;
    wire        rv_ld = pc == 6'd19 || pc == 6'd28 || pc == 6'd32 || pc == 6'd36;

    // a read: the step that takes the value (the address is rd_a, taken below)
    task read(input [5:0] next);
        begin
            ret <= next;
            pc <= P_READ;
        end
    endtask

//
// a corner into poly_c: of a textured face s = u x width (step 40, first clock), then
// t = v x height with the vertex (third clock); of one in one colour 0, 0 and the vertex
// (42). |u|, |v| <= 2^15 and the size <= 256: within 24 bits, the clamp of geom.c never acts.
// j < 4 there.
//
    wire        c_s = (pc == 6'd40 && ms[1:0] == 2'd0) || pc == 6'd42;
    wire        c_tv = (pc == 6'd40 && ms[1:0] == 2'd2) || pc == 6'd42;
    wire [23:0] c_st = textured ? mul_p[23:0] : 24'd0;
    integer     k;
always @(posedge clk) begin
    for(k = 0; k < 4; k = k + 1) if(j[1:0] == k[1:0]) begin
        if(c_s) poly_c[144 * k + 96 +: 24] <= c_st;
        if(c_tv) begin
            poly_c[144 * k + 120 +: 24] <= c_st;
            poly_c[144 * k +: 96] <= v_q;
        end
    end
end

    // the accumulators, in steps 12 and 26: the origin << 8 (12) or 0 at ms 0, the products of
    // the operands taken at ms 1..3 at ms 3..5
    wire        acc_new = ms == 3'd0;
    wire        acc_go = (pc == 6'd12 || pc == 6'd26) && (ms[2] || ms == 3'd3 || acc_new);

always @(posedge clk) begin
    ct_we <= 0;
    // the cache: the line the waiting step needs, from the SDRAM; forgetting every line
    case(cstate)
    C_IDLE: if(inval_req && pc == P_IDLE && !rd_check) begin
        inval_cnt <= 0;
        cstate <= C_INVAL;
    end else if(hold) begin
        mem_valid <= 1;
        mem_addr <= rd_addr[25:5];
        fill_cnt <= 0;
        cstate <= C_MISS;
    end
    C_MISS: if(mem_ready) begin
        mem_valid <= 0;
        cstate <= C_FILL;
    end
    C_FILL: if(mem_rsp_valid) begin             // (cd_we: the word into the RAM)
        fill_cnt <= fill_cnt + 1'b1;
        if(fill_cnt == 3'd7) begin
            ct_we <= 1;
            ct_wa <= rd_addr[11:5];
            ct_wd <= {1'b1, rd_addr[25:12]};
            cstate <= C_SETTLE;
        end
    end
    C_SETTLE: cstate <= C_LOOK;                 // the tag is written now
    C_LOOK: cstate <= C_IDLE;                   // read again: the waiting step goes on
    C_INVAL: begin
        ct_we <= 1;
        ct_wa <= inval_cnt;
        ct_wd <= 0;
        inval_cnt <= inval_cnt + 1'b1;
        if(inval_cnt == 7'd127) cstate <= C_IDLE;
    end
    default: cstate <= C_IDLE;
    endcase

    if(pc != P_IDLE) busy_clocks <= busy_clocks + 1'b1;
    if(wr && wr_addr == 5'd21) busy_clocks <= 0;
    if(wr && wr_addr == 5'd20) faces <= 0;
    if(wr && wr_addr == 5'd22) inval_req <= 1;
    if(cstate == C_INVAL) inval_req <= 0;
    if(poly_valid && poly_ready) poly_valid <= 0;

    if(acc_go) begin
        // (one adder each, its operands chosen: the start value plus 0, or the sum plus a product)
        acc[0] <= (acc_new ? (side ? 40'sd0 : {pq0, 8'd0}) : acc[0]) + (acc_new ? 40'sd0 : {{6{mul_p[33]}}, mul_p});
        acc[1] <= (acc_new ? (side ? 40'sd0 : {pq1, 8'd0}) : acc[1]) + (acc_new ? 40'sd0 : {{6{mul1_p[33]}}, mul1_p});
        acc[2] <= (acc_new ? (side ? 40'sd0 : {pq2, 8'd0}) : acc[2]) + (acc_new ? 40'sd0 : {{6{mul2_p[33]}}, mul2_p});
    end

    if(!hold) begin
    rd_check <= 0;
    rd_addr <= rv_ld ? rv_sum : rd_a;
    cd_ra <= rd_a[11:2];
    ct_ra <= rd_a[11:5];
    case(pc)
    P_IDLE: if(f_count >= 12'd18) begin         // a whole piece is in: word 0 is read
        f_ra <= f_rp[10:0];
        f_rp <= f_rp + 1'b1;
        pk <= 0;
        pc <= P_TAKE;
    end

    P_TAKE: begin                               // word pk - 1 comes out, word pk + 1 is read
        pk <= pk + 1'b1;                        // (words 0..14 go into p0..p2 above)
        if(pk < 5'd17) begin
            f_ra <= f_rp[10:0];
            f_rp <= f_rp + 1'b1;
        end
        if(pk == 5'd16) obj <= f_q[25:0];
        if(pk == 5'd17) blk <= f_q[25:0];
        if(pk == 5'd18) begin
            row <= {f_q[4], f_q[2:0]};
            mirror <= f_q[3];
            phase <= f_q[29:16];
            pc <= 6'd3;
        end
    end

    P_READ: begin                               // the RAMs read: the next step takes the value
        rd_check <= 1;
        pc <= ret;
    end

    P_DONE: pc <= P_IDLE;

    // the block header
    6'd3: read(6'd4);                           // blk + 0x10
    6'd4: begin
        nvert <= rd_word[9:0];
        nbad <= $signed(rd_word) < 0 || $signed(rd_word) > 512;
        read(6'd5);                             // blk + 0x1C
    end
    6'd5: begin verts <= rv_sum; read(6'd6); end            // obj + ..; blk + 0x24
    6'd6: begin uvs <= rv_sum; read(6'd7); end              // obj + ..; blk + 0x28 + 4 row
    6'd7: begin                                 // (rd_addr: verts - 2)
        ep <= rv_sum;                           // blk + the face row's offset
        vi <= 0;
        pc <= nbad || $signed(rd_word) <= 0 ? P_DONE : 6'd8;
    end

    // the vertices into view space
    6'd8: if(vi == nvert) begin nf <= 0; pc <= 6'd16; end
          else read(6'd9);                      // verts + 6 vi
    6'd9:  read(6'd10);                         // (x, y, z into vc)
    6'd10: read(6'd11);
    6'd11: begin ms <= 0; pc <= 6'd12; end
    6'd12: begin                                // the three rows at once: column ms - 1 of each
        ms <= ms + 1'b1;
        mul_a <= vq;
        mul_b <= pq0[17:0];
        if(ms == 3'd5) pc <= 6'd13;
    end
    6'd13: begin                                // t + (s >> 8) into the RAM
        vi <= vi + 1'b1;
        pc <= 6'd8;
    end

    // the face row
    6'd16: if(nf == 10'd1023) pc <= P_DONE;
           else read(6'd17);                    // ep
    6'd17: begin
        if(rd_u16 == 32'hFFFF) pc <= P_DONE;
        else begin
            face <= rv_sum;                     // blk + ..
            ep <= ra_sum;                       // ep + 2
            nf <= nf + 1'b1;
            pc <= 6'd14;
        end
    end
    // (a read whose address comes from the value just read waits a clock: the RAM's output
    // through an adder into its own address register would not make the clock)
    6'd14: read(6'd18);                         // face + 6
    6'd18: begin
        nc <= rd_half[2:0];
        if(rd_s16 < 3 || rd_s16 > 4) pc <= 6'd16;
        else read(6'd19);                       // face + 8
    end
    6'd19: pc <= 6'd15;                         // (rd_addr: verts + the corner's offset)
    6'd15: read(6'd20);
    6'd20: read(6'd21);                         // (x, y, z into vc)
    6'd21: read(6'd22);
    6'd22: read(6'd23);                         // face + 0x14
    6'd23: read(6'd24);                         // face + 0x18 (the normal into nm)
    6'd24: read(6'd25);                         // face + 0x1C
    6'd25: begin ms <= 0; pc <= 6'd26; end
    6'd26: begin                                // ((c0 << 8) - e) . n
        ms <= ms + 1'b1;
        if(ms == 3'd5) pc <= 6'd27;
    end
    6'd27: if(dsum[49] ^ mirror) pc <= 6'd16;   // turned away
           else read(6'd28);                    // face + 4

    // the material
    6'd28: pc <= 6'd47;                         // (rd_addr: mat = mlb + ..)
    6'd47: read(6'd29);                         // mat
    6'd29: begin
        blend <= rd_word == 32'd224;            // see-through through MAPn.SMG
        if(untex) begin
            textured <= 0;
            cut <= 0;
            tw <= 0;
            th <= 0;
            read(6'd33);                        // face + 2
        end else begin
            textured <= 1;
            cut <= rd_word == 32'd64 || rd_word == 32'd48 || rd_word == 32'd224;   // 48: as 64
            read(6'd30);                        // mat + 8
        end
    end
    6'd30: begin tw <= size9(rd_word); read(6'd31); end     // mat + 0xC
    6'd31: begin th <= size9(rd_word); read(6'd32); end     // mat + 4
    6'd32: pc <= 6'd49;                         // (rd_addr: texrec = mlb + ..)
    6'd49: read(6'd45);                         // texrec
    6'd45: begin                                // the frame: frames (if > 0, low half) x phase >> 14
        fmul <= rd_word[31] ? 30'd0 : {14'd0, rd_word[15:0]} * {16'd0, phase};
        pc <= 6'd46;                            // (rd_addr: texrec + 8)
    end
    6'd46: read(6'd34);                         // texrec + 8 + 4 frame
    6'd33: begin colour <= rd_half[7:0]; j <= 0; okv <= 1; pc <= 6'd35; end
    6'd34: begin texdata <= rv_sum; colour <= 0; j <= 0; okv <= 1; pc <= 6'd35; end

    // the corners
    6'd35: if(j == 3'd0 && poly_valid) ;        // the setup unit has not taken the last one
           else if(j == nc) pc <= 6'd44;
           else read(6'd36);                    // face + 8 + 2 j
    6'd36: begin                                // vertex number: vo / 6 = (vo / 2) * 43691 >> 17
        mul_a <= {1'b0, rd_half[15:1]};         // (rd_addr: uvs + vo)
        mul_b <= 18'sd43691;
        ms <= 0;
        pc <= 6'd37;
    end
    6'd37: begin
        ms <= ms + 1'b1;
        if(ms == 3'd2) begin
            v_ra <= mul_p[25:17];
            if(mul_p[30:17] >= {4'd0, nvert}) okv <= 0;
            if(textured) read(6'd38);           // uvs + vo
            else pc <= 6'd41;
        end
    end
    6'd38: begin mul_a <= rd_half; mul_b <= {9'd0, tw}; read(6'd39); end
    6'd39: begin mul_a <= rd_half; mul_b <= {9'd0, th}; ms <= 0; pc <= 6'd40; end
    6'd40: begin                                // u x width (operands of 38), then v x height (39)
        ms <= ms + 1'b1;
        if(ms == 3'd2) begin                    // (c_s at 0, c_tv now: into poly_c)
            j <= j + 1'b1;
            pc <= 6'd35;
        end
    end
    6'd41: pc <= 6'd42;                         // one colour: the vertex comes out of the RAM
    6'd42: begin                                // (c_s, c_tv: the corner into poly_c)
        j <= j + 1'b1;
        pc <= 6'd35;
    end

    // to the setup unit
    6'd44: begin
        if(okv) begin
            poly_valid <= 1;
            poly_n <= nc;
            poly_mode <= {textured, buffer, colour};
            poly_tex <= {6'd0, texdata};
            poly_size <= {cut, blend, 5'd0, th, 7'd0, tw};
            faces <= faces + 1'b1;
        end
        pc <= 6'd16;
    end

    default: pc <= P_IDLE;
    endcase
    end

`ifdef GEOM_STATS
    // (the simulation only) where the clocks go, printed when the CPU clears busy_clocks
    if(wr && wr_addr == 5'd21) begin
        $display("gu: busy %0d, reading %0d (missing %0d), waiting for setup %0d, pieces %0d, vertices %0d, faces %0d, turned away %0d, handed on %0d",
                 busy_clocks, st_read, st_miss, st_setup, st_pieces, st_verts, st_faces, st_away, faces);
        st_read <= 0; st_miss <= 0; st_setup <= 0; st_pieces <= 0; st_verts <= 0; st_faces <= 0; st_away <= 0;
    end else begin
        if(pc == P_READ || hold) st_read <= st_read + 1;
        if(cstate == C_MISS || cstate == C_FILL) st_miss <= st_miss + 1;
        if(pc == 6'd35 && j == 3'd0 && poly_valid) st_setup <= st_setup + 1;
        if(pc == P_TAKE && pk == 5'd18) st_pieces <= st_pieces + 1;
        if(pc == 6'd13) st_verts <= st_verts + 1;
        if(pc == 6'd18 && !hold) st_faces <= st_faces + 1;
        if(pc == 6'd27 && dsum[49]) st_away <= st_away + 1;
    end
`endif
    if(push) f_wp <= f_wp + 1'b1;
    if(reset) begin
        pc <= P_IDLE;
        rd_check <= 0;
        poly_valid <= 0;
        f_wp <= 0;
        f_rp <= 0;
        mem_valid <= 0;
        cstate <= C_INVAL;
        inval_cnt <= 0;
    end
end

endmodule
