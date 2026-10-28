//
// Geometry unit: the pieces of the track (placed meshes) one after the other, bit for bit
// the C model firmware/geom.c (geom.h has the arithmetic). The CPU writes each piece's place
// in view space into a buffer of 2048 words (113 pieces) and goes on with its work; the
// unit takes a piece once all of it is in and reads the mesh from the SDRAM itself (block
// header, vertices, the face
// row for the eye's side, faces, texture coordinates, MLB materials), takes the vertices
// into view space (block RAM, up to 512), leaves out the faces turned away and those of
// routine 224, and hands each other face to the setup unit as a polygon in fixed point.
//
// One multiplier, and two more for the vertices' rows (operands registered, the product three
// clocks after it was issued), and a read cache of 4 KB (direct-mapped, lines of 32 bytes from
// the SDRAM). A step of the sequence below reads a value or works on what it read, or both: a
// read puts its address straight into the cache's RAMs, the next clock waits for them, and the
// step after takes the value when the line is there (a read takes two clocks when it hits);
// on a miss that step waits while the line comes in.
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
//           its block, and a word with [2:0] the face row for the eye's side, [3] a moving
//           part's block (the faces turned away are the others: its frames mirror it, the
//           game's 0x8acf0) and [29:16] the texture phase: of a texture with n frames (texture
//           record +0) frame n x phase >> 14 is drawn (the game's 0xd5980; the frames' data
//           offsets from record +8 on).
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
// read cache: 128 lines of 32 bytes; a read gives the word holding byte address rd_addr
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] cdata [0:1023];
    reg  [14:0] ctag [0:127];                   // {valid, address[25:12]}
    reg  [9:0]  cd_ra, cd_wa;
    reg         cd_we;
    reg  [31:0] cd_wd, cd_q;
    reg  [6:0]  ct_ra, ct_wa;
    reg         ct_we;
    reg  [14:0] ct_wd, ct_q;
always @(posedge clk) begin
    if(cd_we) cdata[cd_wa] <= cd_wd;
    cd_q <= cdata[cd_ra];
    if(ct_we) ctag[ct_wa] <= ct_wd;
    ct_q <= ctag[ct_ra];
end

    localparam  [2:0]   C_IDLE = 3'd0, C_LOOK = 3'd1, C_MISS = 3'd3, C_FILL = 3'd4, C_INVAL = 3'd5,
                        C_SETTLE = 3'd6;
    reg  [2:0]  cstate = C_INVAL;
    reg  [25:0] rd_addr;                        // the value the step in pc waits for (rd_check)
    reg         rd_check = 0;
    reg  [2:0]  fill_cnt;
    reg  [6:0]  inval_cnt = 0;
    wire        rd_hit = ct_q == {1'b1, rd_addr[25:12]};
    wire        hold = rd_check && !rd_hit;     // its line is not there yet: the step waits
    wire [31:0] rd_word = cd_q;

    // the value read: the word, a halfword of it signed or unsigned
    wire [15:0] rd_half = rd_addr[1] ? rd_word[31:16] : rd_word[15:0];
    wire signed [31:0] rd_s16 = {{16{rd_half[15]}}, rd_half};
    wire [31:0] rd_u16 = {16'd0, rd_half};

//
// vertex RAM: 512 vertices in view space, {f, u, r}
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [95:0] vram [0:511];
    reg  [8:0]  v_ra, v_wa;
    reg         v_we;
    reg  [95:0] v_wd, v_q;
always @(posedge clk) begin
    if(v_we) vram[v_wa] <= v_wd;
    v_q <= vram[v_ra];
end

//
// the multipliers
//
    reg  signed [35:0] mul_a, mul1_a, mul2_a;
    reg  signed [17:0] mul_b, mul1_b, mul2_b;
    reg  signed [53:0] mul_p1, mul_p2, mul1_p1, mul1_p2, mul2_p1, mul2_p2;
always @(posedge clk) begin
    mul_p1 <= mul_a * mul_b;
    mul_p2 <= mul_p1;
    mul1_p1 <= mul1_a * mul1_b;                 // the vertices' rows u and f
    mul1_p2 <= mul1_p1;
    mul2_p1 <= mul2_a * mul2_b;
    mul2_p2 <= mul2_p1;
end

    function signed [23:0] sat24(input signed [53:0] v);
        sat24 = v < -54'sd8388608 ? -24'sd8388608 : v > 54'sd8388607 ? 24'sd8388607 : v[23:0];
    endfunction
    function [8:0] size9(input signed [31:0] v);        // 1..256
        size9 = v < 32'sd1 ? 9'd1 : v > 32'sd256 ? 9'd256 : v[8:0];
    endfunction

//
// the sequence
//
    reg  [5:0]  pc = 0, ret;
    reg  [4:0]  pk;                             // word of the piece being taken
    wire [4:0]  pk_m5 = pk - 5'd1, pk_t5 = pk - 5'd10, pk_e5 = pk - 5'd13;
    wire [3:0]  pk_m = pk_m5[3:0];
    wire [1:0]  pk_t = pk_t5[1:0], pk_e = pk_e5[1:0];
    reg  signed [31:0] m [0:8];
    reg  signed [31:0] t [0:2];
    reg  signed [31:0] e [0:2];
    reg  [25:0] obj, blk, verts, uvs, ep, face, mat, texdata, texrec;
    reg  [2:0]  row;
    reg         mirror;
    reg  [13:0] phase;
    reg  [29:0] fmul;                           // frames x phase
    reg  signed [31:0] nvert, ro;
    reg  [9:0]  vi, nf;
    reg  signed [15:0] vx, vy, vz, nx, ny, nz;
    reg  signed [35:0] acc [0:2];
    reg  [3:0]  ms;                             // product steps
    reg  [15:0] c0off, vo;
    reg  [2:0]  nc, j;
    reg         textured, cut, okv;
    reg  [8:0]  tw, th;
    reg  [7:0]  colour;
    reg  signed [23:0] cs;
    reg  signed [53:0] dot;

    localparam  [5:0]   P_IDLE = 6'd0, P_READ = 6'd1, P_DONE = 6'd2, P_TAKE = 6'd48;
`ifdef GEOM_STATS
    integer     st_read = 0, st_miss = 0, st_setup = 0, st_pieces = 0, st_verts = 0, st_faces = 0, st_away = 0;
`endif

    assign busy = pc != P_IDLE || poly_valid || f_count != 0;

    wire [25:0] mat_now = mlb + {{10{rd_half[15]}}, rd_half};
    wire signed [35:0] coord = ms == 4'd0 ? {{20{vx[15]}}, vx} : ms == 4'd1 ? {{20{vy[15]}}, vy} : {{20{vz[15]}}, vz};
    wire signed [35:0] bx = $signed({{12{vx[15]}}, vx, 8'd0}) - {{4{e[0][31]}}, e[0]};
    wire signed [35:0] by = $signed({{12{vy[15]}}, vy, 8'd0}) - {{4{e[1][31]}}, e[1]};
    wire signed [35:0] bz = $signed({{12{vz[15]}}, vz, 8'd0}) - {{4{e[2][31]}}, e[2]};

    // a read: the address into the cache's RAMs, and the step that takes the value
    task read(input [25:0] a, input [5:0] next);
        begin
            rd_addr <= a;
            cd_ra <= {a[11:5], a[4:2]};
            ct_ra <= a[11:5];
            ret <= next;
            pc <= P_READ;
        end
    endtask

always @(posedge clk) begin
    v_we <= 0;
    cd_we <= 0;
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
    C_FILL: if(mem_rsp_valid) begin
        cd_we <= 1;
        cd_wa <= {rd_addr[11:5], fill_cnt};
        cd_wd <= mem_rsp_data;
        fill_cnt <= fill_cnt + 1'b1;
        if(fill_cnt == 3'd7) begin
            ct_we <= 1;
            ct_wa <= rd_addr[11:5];
            ct_wd <= {1'b1, rd_addr[25:12]};
            cstate <= C_SETTLE;
        end
    end
    C_SETTLE: cstate <= C_LOOK;                 // the last word and the tag are written now
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

    if(!hold) begin
    rd_check <= 0;
    case(pc)
    P_IDLE: if(f_count >= 12'd18) begin         // a whole piece is in: word 0 is read
        f_ra <= f_rp[10:0];
        f_rp <= f_rp + 1'b1;
        pk <= 0;
        pc <= P_TAKE;
    end

    P_TAKE: begin                               // word pk - 1 comes out, word pk + 1 is read
        pk <= pk + 1'b1;
        if(pk < 5'd17) begin
            f_ra <= f_rp[10:0];
            f_rp <= f_rp + 1'b1;
        end
        if(pk >= 5'd1 && pk <= 5'd9) m[pk_m] <= f_q;
        if(pk >= 5'd10 && pk <= 5'd12) t[pk_t] <= f_q;
        if(pk >= 5'd13 && pk <= 5'd15) e[pk_e] <= f_q;
        if(pk == 5'd16) obj <= f_q[25:0];
        if(pk == 5'd17) blk <= f_q[25:0];
        if(pk == 5'd18) begin
            row <= f_q[2:0];
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
    6'd3: read(blk + 26'h10, 6'd4);
    6'd4: begin nvert <= rd_word; read(blk + 26'h1C, 6'd5); end
    6'd5: begin verts <= obj + rd_word[25:0]; read(blk + 26'h24, 6'd6); end
    6'd6: begin uvs <= obj + rd_word[25:0]; read(blk + 26'h28 + {21'd0, row, 2'd0}, 6'd7); end
    6'd7: begin
        ro <= rd_word;
        vi <= 0;
        pc <= nvert < 0 || nvert > 512 || $signed(rd_word) <= 0 ? P_DONE : 6'd8;
    end

    // the vertices into view space
    6'd8: if({22'd0, vi} == nvert) begin nf <= 0; ep <= blk + ro[25:0]; pc <= 6'd16; end
          else read(verts + {14'd0, vi, 2'd0} + {15'd0, vi, 1'b0}, 6'd9);
    6'd9:  begin vx <= rd_half; read(verts + {14'd0, vi, 2'd0} + {15'd0, vi, 1'b0} + 26'd2, 6'd10); end
    6'd10: begin vy <= rd_half; read(verts + {14'd0, vi, 2'd0} + {15'd0, vi, 1'b0} + 26'd4, 6'd11); end
    6'd11: begin vz <= rd_half; ms <= 0; pc <= 6'd12; end
    6'd12: begin                                // the three rows at once: column ms of each
        ms <= ms + 1'b1;
        if(ms < 4'd3) begin
            mul_a <= coord;
            mul_b <= m[ms][17:0];
            mul1_a <= coord;
            mul1_b <= m[ms + 4'd3][17:0];
            mul2_a <= coord;
            mul2_b <= m[ms + 4'd6][17:0];
        end
        if(ms == 4'd3) begin
            acc[0] <= mul_p2[35:0];
            acc[1] <= mul1_p2[35:0];
            acc[2] <= mul2_p2[35:0];
        end
        if(ms == 4'd4 || ms == 4'd5) begin
            acc[0] <= acc[0] + mul_p2[35:0];
            acc[1] <= acc[1] + mul1_p2[35:0];
            acc[2] <= acc[2] + mul2_p2[35:0];
        end
        if(ms == 4'd5) pc <= 6'd13;
    end
    6'd13: begin
        v_we <= 1;
        v_wa <= vi[8:0];
        v_wd <= {t[2] + {{4{acc[2][35]}}, acc[2][35:8]}, t[1] + {{4{acc[1][35]}}, acc[1][35:8]},
                 t[0] + {{4{acc[0][35]}}, acc[0][35:8]}};
        vi <= vi + 1'b1;
        pc <= 6'd8;
    end

    // the face row
    6'd16: if(nf == 10'd1023) pc <= P_DONE;
           else read(ep, 6'd17);
    6'd17: begin
        if(rd_u16 == 32'hFFFF) pc <= P_DONE;
        else begin
            face <= blk + {10'd0, rd_half};
            ep <= ep + 26'd2;
            nf <= nf + 1'b1;
            pc <= 6'd14;
        end
    end
    // (a read whose address comes from the value just read waits a clock: the RAM's output
    // through an adder into its own address register would not make the clock)
    6'd14: read(face + 26'h6, 6'd18);
    6'd18: begin
        nc <= rd_half[2:0];
        if(rd_s16 < 3 || rd_s16 > 4) pc <= 6'd16;
        else read(face + 26'h8, 6'd19);
    end
    6'd19: begin c0off <= rd_half; pc <= 6'd15; end
    6'd15: read(verts + {10'd0, c0off}, 6'd20);
    6'd20: begin vx <= rd_half; read(verts + {10'd0, c0off} + 26'd2, 6'd21); end
    6'd21: begin vy <= rd_half; read(verts + {10'd0, c0off} + 26'd4, 6'd22); end
    6'd22: begin vz <= rd_half; read(face + 26'h14, 6'd23); end
    6'd23: begin nx <= rd_half; read(face + 26'h18, 6'd24); end
    6'd24: begin ny <= rd_half; read(face + 26'h1C, 6'd25); end
    6'd25: begin nz <= rd_half; ms <= 0; pc <= 6'd26; end
    6'd26: begin                                // ((c0 << 8) - e) . n
        ms <= ms + 1'b1;
        case(ms)
        4'd0: begin mul_a <= bx; mul_b <= {{2{nx[15]}}, nx}; end
        4'd1: begin mul_a <= by; mul_b <= {{2{ny[15]}}, ny}; end
        4'd2: begin mul_a <= bz; mul_b <= {{2{nz[15]}}, nz}; end
        4'd3: dot <= mul_p2;
        4'd4: dot <= dot + mul_p2;
        4'd5: begin dot <= dot + mul_p2; pc <= 6'd27; end
        default: ;
        endcase
    end
    6'd27: if(dot[53] ^ mirror) pc <= 6'd16;    // turned away
           else read(face + 26'h4, 6'd28);

    // the material
    6'd28: begin mat <= mat_now; pc <= 6'd47; end
    6'd47: read(mat, 6'd29);
    6'd29: begin
        if(rd_word == 32'd224) pc <= 6'd16;     // see-through through MAPn.SMG: not yet
        else if(rd_word == 32'd128) begin
            textured <= 0;
            cut <= 0;
            tw <= 0;
            th <= 0;
            read(face + 26'h2, 6'd33);
        end else begin
            textured <= 1;
            cut <= rd_word == 32'd64;
            read(mat + 26'h8, 6'd30);
        end
    end
    6'd30: begin tw <= size9(rd_word); read(mat + 26'hC, 6'd31); end
    6'd31: begin th <= size9(rd_word); read(mat + 26'h4, 6'd32); end
    6'd32: begin texrec <= mlb + rd_word[25:0]; pc <= 6'd49; end
    6'd49: read(texrec, 6'd45);
    6'd45: begin                                // the frame: frames (if > 0, low half) x phase >> 14
        fmul <= rd_word[31] ? 30'd0 : {14'd0, rd_word[15:0]} * {16'd0, phase};
        pc <= 6'd46;
    end
    6'd46: read(texrec + 26'h8 + {8'd0, fmul[29:14], 2'd0}, 6'd34);
    6'd33: begin colour <= rd_half[7:0]; j <= 0; okv <= 1; pc <= 6'd35; end
    6'd34: begin texdata <= mlb + rd_word[25:0]; colour <= 0; j <= 0; okv <= 1; pc <= 6'd35; end

    // the corners
    6'd35: if(j == 3'd0 && poly_valid) ;        // the setup unit has not taken the last one
           else if(j == nc) pc <= 6'd44;
           else read(face + 26'h8 + {22'd0, j, 1'b0}, 6'd36);
    6'd36: begin                                // vertex number: vo / 6 = (vo / 2) * 43691 >> 17
        vo <= rd_half;
        mul_a <= {21'd0, rd_half[15:1]};
        mul_b <= 18'sd43691;
        ms <= 0;
        pc <= 6'd37;
    end
    6'd37: begin
        ms <= ms + 1'b1;
        if(ms == 4'd2) begin
            v_ra <= mul_p2[25:17];
            if({18'd0, mul_p2[30:17]} >= nvert) okv <= 0;
            if(textured) read(uvs + {10'd0, vo}, 6'd38);
            else pc <= 6'd41;
        end
    end
    6'd38: begin mul_a <= {{20{rd_half[15]}}, rd_half}; mul_b <= {9'd0, tw}; read(uvs + {10'd0, vo} + 26'd2, 6'd39); end
    6'd39: begin mul_a <= {{20{rd_half[15]}}, rd_half}; mul_b <= {9'd0, th}; ms <= 0; pc <= 6'd40; end
    6'd40: begin                                // u x width three clocks after 38, then v x height
        ms <= ms + 1'b1;
        if(ms == 4'd0) cs <= sat24(mul_p2);
        if(ms == 4'd2) begin
            poly_c[144 * j +: 144] <= {sat24(mul_p2), cs, v_q};
            j <= j + 1'b1;
            pc <= 6'd35;
        end
    end
    6'd41: pc <= 6'd42;                         // one colour: the vertex comes out of the RAM
    6'd42: begin
        poly_c[144 * j +: 144] <= {48'd0, v_q};
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
            poly_size <= {cut, 6'd0, th, 7'd0, tw};
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
        if(pc == 6'd27 && dot[53]) st_away <= st_away + 1;
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
