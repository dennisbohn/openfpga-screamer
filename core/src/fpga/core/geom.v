//
// Geometry unit (GU): takes the pieces of the track one after the other, transforms their
// meshes into view space and hands polygons to the setup unit (setup.v). It is bit for bit
// the C model firmware/geom.c; geom.h has the arithmetic. A piece is one placement of a mesh.
//
// Work flow
//   1. The CPU writes a piece (19 words, see the registers below) into a buffer of 2048
//      words (107 pieces) and goes on with its work. soc.v holds a write back only while
//      the buffer is full.
//   2. The unit takes a piece once all of it is in the buffer. It reads the mesh from the
//      SDRAM itself: the block header, the vertices, the face row for the eye's side, the
//      faces, the texture coordinates and the materials of the MLB file.
//   3. It takes the vertices into view space (docs/original_geometry.md 5) and keeps them
//      in a block RAM (up to 512 vertices).
//   4. It leaves out the faces turned away (docs/original_geometry.md 6.2) and hands each
//      other face to the setup unit as a polygon in fixed point.
//
// Special faces
//   Routine 224 is handled like routine 16 (texture) with the size bits 30 (blended) and
//   31 (texel 0 see-through) set, see raster.v.
//   A block with flag 0x2000 (a car's body) has a normal per vertex. After the vertices
//   the unit works out each vertex's reflection u, v for routine 160, the car's windows
//   (docs/original_geometry.md 8.5, geom.h has the steps). They go into a RAM of 512
//   entries. The faces of routine 160 take their texel coordinates from it. The texture
//   wraps at 256 (size bit 29).
//
// Polygon handed to the setup unit (poly_* ports)
//   poly_n      3 or 4 corners
//   poly_mode   [7:0] colour, [8] buffer, [9] textured
//   poly_tex    SDRAM byte address of the texture data
//   poly_size   [31] texel 0 see-through, [30] blended, [29] wrap, [24:16] texture height,
//               [8:0] texture width
//   poly_c      corner k at [144 k +: 144] = {t[23:0], s[23:0], f[31:0], u[31:0], r[31:0]}.
//               r, u, f are view space coordinates (right, up, forward) with 8 fraction
//               bits, s and t the texel coordinates with 8 fraction bits.
//
// Sequence
//   The unit is a sequencer: pc is the step. A step reads a value, works on what it read,
//   or both. A read puts its address straight into the cache's RAMs, the next clock (step
//   P_READ) waits for them, and the step after takes the value when the line is there. A
//   read takes two clocks when it hits. On a miss, the step that takes the value waits
//   (hold) while the line comes in.
//   Steps (the numbers are the values of pc):
//     0 P_IDLE        wait for a whole piece in the buffer
//     48 P_TAKE       take the piece's words out of the buffer into the small RAMs
//     1 P_READ        the wait clock of a read;  2 P_DONE  end of the piece
//     3..7, 57, 58    block header (57 is R_FLAGS, 58 is R_NRM): vertex count, flags,
//                     addresses of the vertices, the normals and the texture coordinates,
//                     address of the face row
//     8..13           for each vertex: read x, y, z (8..11), transform (12, six clocks, the
//                     three rows at once), store it in the vertex RAM (13)
//     50..56          if the block has normals: for each vertex with a normal, its
//                     reflection (R_START .. R_N2, R_C), using step 12 for the normal
//     16, 17          next face of the face row (the row ends with 0xFFFF)
//     14, 18, 19, 15  number of corners (3 or 4 only), address of the first corner's vertex
//     20..22          the first corner's vertex into vc;  22..25 the face's normal into nm
//     26, 27          side test ((first corner << 8) - eye) . normal; a face turned away
//                     goes back to step 16
//     28, 47, 29      material kind (128 or 192: one colour)
//     30, 31          texture width and height
//     32, 49, 45, 46  texture record and the frame of the texture
//     33, 34          colour (one colour) or texture data address
//     35..42          the corners: for each corner its vertex and texel coordinates
//     44              hand the polygon to the setup unit
//
// Parts
//   Multipliers: mul (a vertex's row r, the vertex number of an offset, the texture
//   coordinates) and mul1, mul2 (the rows u and f, and the side test). Their operands are
//   registered and the product comes two clocks after it was issued.
//   Read cache: 4 KB, direct-mapped, 128 lines of 32 bytes, filled from the SDRAM over the
//   line port (mem_*), which soc.v shares with the rasterizer.
//   Read addresses come from one adder (a base register plus an offset, the base often the
//   address read before) or, for an address made of a value just read (a base plus a word
//   or a halfword from the mesh), from a second one.
//   Small block RAMs hold the piece's matrix, origin and eye, and the corner and the
//   normal of the side test.
//
// Registers (soc.v maps them at 0xF000_3100 + 4 * wr_addr, docs/soc.md):
//   17      SDRAM byte address of the MLB      18  [8] the buffer drawn into
//   20      faces handed on since the last clear (write: clear)
//   21      clocks busy since the last clear (write: clear)
//   22      write: forget the cached lines (after the mesh data changed)
//   23      write: the next word of a piece into the buffer (soc.v holds the write back while
//           it is full). A piece is 19 words:
//             0..8    local to view matrix: rows r, u, f, each with the columns x, y, z;
//                     16 fraction bits, within +-2^17
//             9..11   the piece's origin in view space, 8 fraction bits
//             12..14  the eye in the piece's frame, 8 fraction bits
//             15, 16  SDRAM byte addresses of the mesh's object and of its block
//             17      [4] and [2:0] the face row for the eye's side (0..15, read at block
//                     +0x28 + 4 x row; [4] counts 8: the eye is below the block's x-z plane,
//                     original: STARTL.EXE 0x8a598),
//                     [3] a moving part's block (the faces turned away are the others, its
//                     frames mirror it, original: STARTL.EXE 0x8acf0),
//                     [29:16] the texture phase: of a texture with n frames (texture
//                     record +0), frame n x phase >> 14 is drawn (original: STARTL.EXE
//                     0xd5980; the frames' data offsets start at record +8)
//             18      for routine 160: [15:0] the reflection picture's phase (frame
//                     n x phase >> 16), [23:16] and [31:24] the reflection's offsets of
//                     u and v
//

module geom (
    input   wire            clk,
    input   wire            reset,

    // register writes of the CPU, wr_addr is the register number
    input   wire            wr,
    input   wire    [4:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            busy,               // a piece is in the buffer or in work, or a polygon waits
    output  wire            full,               // the piece buffer takes no word now
    output  reg     [31:0]  faces = 0,          // register 20
    output  reg     [31:0]  busy_clocks = 0,    // register 21

    // Line requests to the SDRAM (soc.v shares the line port with the other units).
    // mem_valid stays high with mem_addr (the 32-byte line) until mem_ready. The line then
    // comes as 8 words in order, each with mem_rsp_valid.
    output  reg             mem_valid = 0,
    input   wire            mem_ready,
    output  reg     [25:5]  mem_addr,
    input   wire            mem_rsp_valid,
    input   wire    [31:0]  mem_rsp_data,

    // a polygon for the setup unit: taken in a clock with poly_valid and poly_ready
    // (the formats are in the header)
    output  reg             poly_valid = 0,
    input   wire            poly_ready,
    output  reg     [2:0]   poly_n,
    output  reg     [9:0]   poly_mode,          // [7:0] colour, [8] buffer, [9] textured
    output  reg     [31:0]  poly_tex,
    output  reg     [31:0]  poly_size,
    output  reg     [575:0] poly_c              // corner k at [144 k +: 144]: {t, s, f, u, r}
);

//
// The registers the CPU writes, and the piece buffer: a FIFO of 2048 words in block RAM,
// written by the CPU (push) and read by the sequence (f_ra, f_q).
//
    reg  [25:0] mlb;                            // register 17: the MLB file's address
    reg         buffer;                         // register 18: the buffer drawn into
    reg         inval_req = 1;                  // forget the cached lines (also after power-up)
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] fifo [0:2047];
    reg  [11:0] f_wp = 0, f_rp = 0;             // write and read pointer, a wrap bit over the address
    reg  [10:0] f_ra;                           // read address of the FIFO
    reg  [31:0] f_q;                            // its read data, one clock after f_ra
    wire [11:0] f_count = f_wp - f_rp;          // words in the FIFO
    wire        push = wr && wr_addr == 5'd23 && !reset;
    assign full = f_count >= 12'd2046;          // room for the write soc.v may have under way

    // CPU register writes, the FIFO's write port and read port
always @(posedge clk) begin
    if(wr && wr_addr == 5'd17) mlb <= wr_data[25:0];
    if(wr && wr_addr == 5'd18) buffer <= wr_data[8];
    if(push) fifo[f_wp[10:0]] <= wr_data;
    f_q <= fifo[f_ra];
end

//
// The sequence's registers
//
    reg  [5:0]  pc = 0, ret;                    // the step; ret: the step that takes a read's value
    reg  [4:0]  pk;                             // word of the piece being taken
    reg  [25:0] obj, blk,                       // SDRAM addresses of the piece's mesh object and block
                verts, uvs,                     // addresses of the block's vertices and texture coordinates
                ep, face,                       // next entry of the face row; the current face
                texdata;                        // address of the face's texture data
    reg  [3:0]  row;                            // the face row for the eye's side
    reg         mirror;                         // a moving part's block
    reg  [13:0] phase;                          // the texture phase
    reg  [15:0] rphase;                         // routine 160's picture
    reg  [7:0]  offx, offz;                     // and the reflection's offsets
    reg  [31:0] fmul;                           // frames x phase (16 bits)
    reg         refl;                           // a face of routine 160
    reg         refl_on;                        // the block has normals (flag 0x2000)
    reg  [25:0] nrm;                            // their table: count, then x, y, z as floats
    reg  [9:0]  ncnt;                           // vertices with a normal (up to nvert)
    reg         nmac;                           // step 12 turns a normal, not a vertex
    reg  [9:0]  nvert;                          // 0..512 once step 7 let the piece through
    reg         nbad;                           // the block's vertex count is not 0..512
    reg  [9:0]  vi, nf;                         // vertex number; face number in the row
    reg  signed [39:0] acc [0:2];               // the origin << 8 plus a row's products
    reg  [2:0]  ms;                             // product steps
    reg  [2:0]  nc, j;                          // corners of the face; the corner being done
    reg  [3:0]  rs;                             // R_C's steps
    reg         textured, cut, blend, okv;      // okv: every vertex number so far is below nvert
    reg  [8:0]  tw, th;                         // texture width and height, 1..256 (0 in one colour)
    reg  [7:0]  colour;

    // main steps with names; the others are numbered (see the header)
    localparam  [5:0]   P_IDLE = 6'd0, P_READ = 6'd1, P_DONE = 6'd2, P_TAKE = 6'd48;
    // The reflections: the table's count, then per vertex its normal's three words, the turn
    // into view space (step 12) and the steps of R_C (rs 0..12). R_FLAGS and R_NRM are two
    // steps of the block header.
    localparam  [5:0]   R_START = 6'd50, R_CNT = 6'd51, R_V = 6'd52, R_N0 = 6'd53, R_N1 = 6'd54,
                        R_N2 = 6'd55, R_C = 6'd56, R_FLAGS = 6'd57, R_NRM = 6'd58;

//
// Read cache: 128 lines of 32 bytes, direct-mapped. A read gives the word that holds the
// byte address rd_addr, one clock after the address is set. The state machine cstate
// fetches a missing line and forgets all lines on request.
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [31:0] cdata [0:1023];
    reg  [14:0] ctag [0:127];                   // {valid, address[25:12]}
    reg  [9:0]  cd_ra;
    reg  [31:0] cd_q;
    reg  [6:0]  ct_ra, ct_wa;
    reg         ct_we;
    reg  [14:0] ct_wd, ct_q;

    // states of the cache: C_IDLE nothing to do (a miss starts C_MISS); C_MISS the line is
    // requested; C_FILL its words come in; C_SETTLE the tag is written; C_LOOK the line is
    // read again; C_INVAL every tag is cleared, one per clock
    localparam  [2:0]   C_IDLE = 3'd0, C_LOOK = 3'd1, C_MISS = 3'd3, C_FILL = 3'd4, C_INVAL = 3'd5,
                        C_SETTLE = 3'd6;
    reg  [2:0]  cstate = C_INVAL;
    reg  [25:0] rd_addr;                        // the value the step in pc waits for (rd_check)
    reg         rd_check = 0;                   // rd_addr was read: its line must be in the cache
    reg  [2:0]  fill_cnt;                       // the word of the line coming in
    reg  [6:0]  inval_cnt = 0;                  // the tag being cleared
    // The words of a line go into the RAM as they come in (the step waits until C_LOOK has
    // read the line again). The tag is written a clock after the last word.
    wire        cd_we = cstate == C_FILL && mem_rsp_valid;
    // data and tag RAMs: write port (line fill, tag write) and read port (cd_q, ct_q)
always @(posedge clk) begin
    if(cd_we) cdata[{rd_addr[11:5], fill_cnt}] <= mem_rsp_data;
    cd_q <= cdata[cd_ra];
    if(ct_we) ctag[ct_wa] <= ct_wd;
    ct_q <= ctag[ct_ra];
end
    wire        rd_hit = ct_q == {1'b1, rd_addr[25:12]};
    wire        hold = rd_check && !rd_hit;     // its line is not there yet: the step waits
    wire [31:0] rd_word = cd_q;

    // The value read: the word, or a halfword of it signed or unsigned.
    wire [15:0] rd_half = rd_addr[1] ? rd_word[31:16] : rd_word[15:0];
    wire signed [31:0] rd_s16 = {{16{rd_half[15]}}, rd_half};
    wire [31:0] rd_u16 = {16'd0, rd_half};

//
// Vertex RAM: 512 vertices in view space, {f, u, r} (32 bits each, 8 fraction bits), written
// from the accumulators in step 13 (which waits for no read). The accumulators have 16
// fraction bits, so the low 8 bits are dropped.
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [95:0] vram [0:511];
    reg  [8:0]  v_ra;
    reg  [95:0] v_q;
    // write port at step 13, read port (v_q) one clock after v_ra
always @(posedge clk) begin
    if(pc == 6'd13) vram[vi[8:0]] <= {acc[2][39:8], acc[1][39:8], acc[0][39:8]};
    v_q <= vram[v_ra];
end

//
// Reflection RAM: each vertex's {v, u} of routine 160 (10 bits signed each, whole texels),
// written at the end of R_C and read with the vertex (v_ra). It is kept from block to block,
// like the original's table.
//
    (* ramstyle = "no_rw_check, M10K" *) reg  [19:0] rram [0:511];
    reg  [19:0] r_q;
    wire [19:0] r_wd;
    wire        r_we = pc == R_C && rs == 4'd12;    // R_C waits for no read
    // write port (r_we) and read port (r_q)
always @(posedge clk) begin
    if(r_we) rram[vi[8:0]] <= r_wd;
    r_q <= rram[v_ra];
end

//
// Small RAMs for the piece's matrix, origin and eye, and for the corner and the normal of
// the side test:
//   p0, p1, p2  one for each row of the matrix (r, u, f). Words 0..2 hold the columns x, y, z
//               (the low 18 bits are used), word 3 the origin's coordinate. Words 4..6 of p0
//               hold the eye (x, y, z).
//   vc          a vertex, or the face's first corner (x, y, z as read), or a vertex's normal
//               (rounded to Q14, in R_N0 .. R_N2).
//   nm          the face's normal.
// The piece goes in as it is taken (no read is waited for then). All RAMs are read with p_ra
// of the clock before. The vertex transform reads the origin (step 11, for the accumulators
// in the first clock of step 12), then the columns 0..2 for the next three clocks of step 12.
// The side test reads the eye, the corner and the normal (step 26, for its clocks 1..3).
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
    wire        side = pc[4];                   // 1 in steps 25, 26 (side test); 0 in 11, 12 (vertex
                                                // transform)
    wire [2:0]  p_ra = {side, pc == 6'd12 || pc == 6'd26 ? ms[1:0] : 2'd3};
    wire        n_rd = pc == R_N0 || pc == R_N1 || pc == R_N2;     // a normal's word
    wire        v_we = !hold && (pc == 6'd9 || pc == 6'd10 || pc == 6'd11 ||
                                 pc == 6'd20 || pc == 6'd21 || pc == 6'd22 || n_rd);
    // A normal's component from its float bits, rounded to Q14 (14 fraction bits): exponent
    // 112..127 (else 0), {1, mantissa[22:8]} >> (15 - e[3:0]), plus one, halved.
    wire [15:0] q_m = {1'b1, rd_word[22:8]} >> ~rd_word[26:23];
    wire [16:0] q_r = {1'b0, q_m} + 17'd1;
    wire [15:0] q_a = rd_word[30:27] == 4'b0111 ? q_r[16:1] : 16'd0;
    wire [15:0] q14 = rd_word[31] ? -q_a : q_a;
    wire        n_we = !hold && (pc == 6'd23 || pc == 6'd24 || pc == 6'd25);
    // Where a word goes. In P_TAKE the word pk - 1 comes out of the FIFO: p_we selects the
    // matrix RAM (p0, p1, p2) and p_wa its address. vn_wa is the address in vc and nm.
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
    6'd10, 6'd21, 6'd24, R_N1: vn_wa = 2'd1;
    6'd11, 6'd22, 6'd25, R_N2: vn_wa = 2'd2;
    default:                   vn_wa = 2'd0;
    endcase
end
    // write ports of the small RAMs and their registered read ports
always @(posedge clk) begin
    if(p_we[0]) p0[p_wa] <= f_q;
    if(p_we[1]) p1[p_wa[1:0]] <= f_q;
    if(p_we[2]) p2[p_wa[1:0]] <= f_q;
    if(v_we) vc[vn_wa] <= n_rd ? q14 : rd_half;
    if(n_we) nm[vn_wa] <= rd_half;
    pq0 <= p0[p_ra];
    pq1 <= p1[p_ra[1:0]];
    pq2 <= p2[p_ra[1:0]];
    vq <= vc[p_ra[1:0]];
    nq <= nm[p_ra[1:0]];
end

//
// The multipliers
//
    // mul computes a vertex's row r, the vertex number of an offset and the texture
    // coordinates. mul1 and mul2 compute the rows u and f. In the side test they multiply
    // the normal with b = (c0 << 8) - e (33 bits: an int16 << 8 less an int32), in two
    // halves: b = b[32:17] 2^17 + b[16:0]. In R_C they compute the reflection's products
    // (below). Every product is below 2^33. Steps 37 and 40 take a product while its
    // operands are still unchanged.
    reg  signed [17:0] mul_a, mul_b, mul1_a, mul1_b, mul2_a, mul2_b;
    reg  signed [33:0] mul_p, mul1_p, mul2_p;
    wire signed [32:0] sb = {{9{vq[15]}}, vq, 8'd0} - {pq0[31], pq0};

//
// The reflection's datapath (geom.h):
//   p  the vertex from the RAM less EYE_BACK in f, shifted by pl (the bit length of the
//      largest |component|, one's complement) to 17 bits. One component is shifted per
//      clock (R_N1, R_N2, the first clock of step 12).
//   n  the normal, turned into view space by step 12, in the accumulators.
// The steps rs of R_C then compute:
//   0  S's products p n      1  Q's products p p      2  S (their sum)
//   3  Q, y0 from the table, and T = S n for r and f
//   4  t1      5  B = p - (T >> 15)      6  t2      8  y
//   10 B y for u and v       12 u and v into the reflection RAM
//
    wire signed [31:0] p_r = v_q[31:0], p_u = v_q[63:32], p_f = v_q[95:64] - 32'd32768;
    wire [31:0] p_o = (p_r ^ {32{p_r[31]}}) | (p_u ^ {32{p_u[31]}}) | (p_f ^ {32{p_f[31]}});
    function [4:0] blen(input [31:0] o);        // the bit length of o (below 2^31)
        integer b;
        begin
            blen = 5'd0;
            for(b = 0; b < 31; b = b + 1) if(o[b]) blen = b[4:0] + 5'd1;
        end
    endfunction
    function [15:0] rsq(input [6:0] i);         // 2^16 / sqrt((i + 0.5) / 32), i = 32..127
        begin
            case(i)
            7'd32: rsq = 16'd65030; 7'd33: rsq = 16'd64052; 7'd34: rsq = 16'd63117; 7'd35: rsq = 16'd62222;
            7'd36: rsq = 16'd61363; 7'd37: rsq = 16'd60540; 7'd38: rsq = 16'd59748; 7'd39: rsq = 16'd58987;
            7'd40: rsq = 16'd58254; 7'd41: rsq = 16'd57548; 7'd42: rsq = 16'd56867; 7'd43: rsq = 16'd56210;
            7'd44: rsq = 16'd55574; 7'd45: rsq = 16'd54960; 7'd46: rsq = 16'd54366; 7'd47: rsq = 16'd53791;
            7'd48: rsq = 16'd53233; 7'd49: rsq = 16'd52693; 7'd50: rsq = 16'd52169; 7'd51: rsq = 16'd51660;
            7'd52: rsq = 16'd51165; 7'd53: rsq = 16'd50685; 7'd54: rsq = 16'd50218; 7'd55: rsq = 16'd49763;
            7'd56: rsq = 16'd49321; 7'd57: rsq = 16'd48890; 7'd58: rsq = 16'd48470; 7'd59: rsq = 16'd48061;
            7'd60: rsq = 16'd47663; 7'd61: rsq = 16'd47273; 7'd62: rsq = 16'd46894; 7'd63: rsq = 16'd46523;
            7'd64: rsq = 16'd46161; 7'd65: rsq = 16'd45807; 7'd66: rsq = 16'd45462; 7'd67: rsq = 16'd45124;
            7'd68: rsq = 16'd44793; 7'd69: rsq = 16'd44470; 7'd70: rsq = 16'd44153; 7'd71: rsq = 16'd43843;
            7'd72: rsq = 16'd43540; 7'd73: rsq = 16'd43243; 7'd74: rsq = 16'd42951; 7'd75: rsq = 16'd42666;
            7'd76: rsq = 16'd42386; 7'd77: rsq = 16'd42112; 7'd78: rsq = 16'd41843; 7'd79: rsq = 16'd41579;
            7'd80: rsq = 16'd41320; 7'd81: rsq = 16'd41065; 7'd82: rsq = 16'd40816; 7'd83: rsq = 16'd40571;
            7'd84: rsq = 16'd40330; 7'd85: rsq = 16'd40093; 7'd86: rsq = 16'd39861; 7'd87: rsq = 16'd39632;
            7'd88: rsq = 16'd39408; 7'd89: rsq = 16'd39187; 7'd90: rsq = 16'd38970; 7'd91: rsq = 16'd38756;
            7'd92: rsq = 16'd38546; 7'd93: rsq = 16'd38340; 7'd94: rsq = 16'd38136; 7'd95: rsq = 16'd37936;
            7'd96: rsq = 16'd37739; 7'd97: rsq = 16'd37545; 7'd98: rsq = 16'd37354; 7'd99: rsq = 16'd37166;
            7'd100: rsq = 16'd36980; 7'd101: rsq = 16'd36798;
            7'd102: rsq = 16'd36618; 7'd103: rsq = 16'd36441;
            7'd104: rsq = 16'd36266; 7'd105: rsq = 16'd36093;
            7'd106: rsq = 16'd35924; 7'd107: rsq = 16'd35756;
            7'd108: rsq = 16'd35591; 7'd109: rsq = 16'd35428;
            7'd110: rsq = 16'd35267; 7'd111: rsq = 16'd35109;
            7'd112: rsq = 16'd34953; 7'd113: rsq = 16'd34798;
            7'd114: rsq = 16'd34646; 7'd115: rsq = 16'd34496;
            7'd116: rsq = 16'd34347; 7'd117: rsq = 16'd34201;
            7'd118: rsq = 16'd34056; 7'd119: rsq = 16'd33913;
            7'd120: rsq = 16'd33772; 7'd121: rsq = 16'd33633;
            7'd122: rsq = 16'd33496; 7'd123: rsq = 16'd33360;
            7'd124: rsq = 16'd33225; 7'd125: rsq = 16'd33093;
            7'd126: rsq = 16'd32962; 7'd127: rsq = 16'd32832;
            default: rsq = 16'd65030;
            endcase
        end
    endfunction
    reg  [4:0]  pl;
    reg  signed [16:0] pn0, pn1, pn2;
    reg  signed [17:0] sh, br, bf;
    reg  [16:0] qx;
    reg  [15:0] y0;
    reg         hq;
    wire signed [31:0] p_sel = pc == R_N1 ? p_r : pc == R_N2 ? p_u : p_f;
    wire signed [47:0] p_sh = {p_sel, 16'd0} >>> pl;
    wire signed [17:0] nv0 = acc[0][31:14], nv1 = acc[1][31:14], nv2 = acc[2][31:14];
    wire signed [35:0] psum = {{2{mul_p[33]}}, mul_p} + {{2{mul1_p[33]}}, mul1_p} + {{2{mul2_p[33]}}, mul2_p};
    wire signed [19:0] br_d = {{3{pn0[16]}}, pn0} - {mul1_p[33], mul1_p[33:15]};    // p - (T >> 15)
    wire signed [19:0] bf_d = {{3{pn2[16]}}, pn2} - {mul2_p[33], mul2_p[33:15]};
    wire        q_lo = psum[33:32] == 2'd0;     // Q < 2^32: Q' = 4 Q
    wire [33:0] q_n = q_lo ? {psum[31:0], 2'd0} : psum[33:0];
    wire signed [19:0] t3 = (20'sd196608 - $signed({1'b0, mul_p[33:15]})) >>> 1;
    wire signed [19:0] t3c = t3 - (t3 >>> 7);
    wire signed [17:0] yq = mul_p[33:16];
    wire signed [35:0] pu = (hq ? {mul1_p[33], mul1_p, 1'b0} : {{2{mul1_p[33]}}, mul1_p}) + 36'sd16777216;
    wire signed [35:0] pv = (hq ? {mul2_p[33], mul2_p, 1'b0} : {{2{mul2_p[33]}}, mul2_p}) + 36'sd16777216;
    assign r_wd = {pv[34:25] + 10'd127 - {2'd0, offz}, pu[34:25] + 10'd127 - {2'd0, offx}};
    // the reflection's registers, each taken in its step
always @(posedge clk) begin
    if(pc == R_N0) pl <= blen(p_o);
    if(pc == R_N1) pn0 <= p_sh[16:0];
    if(pc == R_N2) pn1 <= p_sh[16:0];
    if(pc == 6'd12 && nmac && ms == 3'd0) pn2 <= p_sh[16:0];
    if(pc == R_C) case(rs)
    4'd2: sh <= psum[33:16];
    4'd3: begin qx <= q_n[33:17]; y0 <= rsq(q_n[33:27]); hq <= q_lo; end
    4'd5: begin br <= br_d[17:0]; bf <= bf_d[17:0]; end
    default: ;
    endcase
end

    // The multipliers and the choice of their operands. The operands are taken every clock;
    // only the products of steps 12 and 26 (two clocks on) and those of R_C are used.
always @(posedge clk) begin
    mul_p <= mul_a * mul_b;
    mul1_p <= mul1_a * mul1_b;
    mul2_p <= mul2_a * mul2_b;
    if(pc == R_C && (rs == 4'd0 || rs == 4'd1)) begin  // S: p n, Q: p p
        mul1_a <= {pn1[16], pn1}; mul1_b <= rs[0] ? {pn1[16], pn1} : nv1;
        mul2_a <= {pn2[16], pn2}; mul2_b <= rs[0] ? {pn2[16], pn2} : nv2;
    end else if(pc == R_C && rs == 4'd3) begin  // T: S n_r, S n_f
        mul1_a <= sh; mul1_b <= nv0;
        mul2_a <= sh; mul2_b <= nv2;
    end else if(pc == R_C && rs == 4'd10) begin // B y
        mul1_a <= br; mul1_b <= yq;
        mul2_a <= bf; mul2_b <= yq;
    end else begin
        mul1_a <= side ? {{2{sb[32]}}, sb[32:17]} : {{2{vq[15]}}, vq};
        mul1_b <= side ? {{2{nq[15]}}, nq} : pq1[17:0];
        mul2_a <= side ? {1'b0, sb[16:0]} : {{2{vq[15]}}, vq};
        mul2_b <= side ? {{2{nq[15]}}, nq} : pq2[17:0];
    end
end

    // The side test's sum b . n = acc1 2^17 + acc2 (|acc1| < 2^32, |acc2| < 2^34). Its sign
    // (bit 49) says on which side of the face the eye is.
    wire [49:0] dsum = {acc[1][32:0], 17'd0} + {{15{acc[2][34]}}, acc[2][34:0]};

    function [8:0] size9(input signed [31:0] v);        // a texture size limited to 1..256
        size9 = v < 32'sd1 ? 9'd1 : v > 32'sd256 ? 9'd256 : v[8:0];
    endfunction

`ifdef GEOM_STATS
    // counters for the simulation: where the clocks go (printed below)
    integer     st_read = 0, st_miss = 0, st_setup = 0, st_pieces = 0, st_verts = 0, st_faces = 0,
                st_away = 0, st_refl = 0;
`endif

    assign busy = pc != P_IDLE || poly_valid || f_count != 0;

//
// The address adders.
//   ra_sum = ra_base + ra_off: a base plus an offset (19 bits, two's complement). It is the
//   next read's address. Where a read follows one of the same record, the base is the
//   address read before (rd_addr). Every clock that waits for no line takes ra_sum into
//   rd_addr and into the cache's read address, also where nothing is read. Where rd_addr
//   must stand, the offset is 0: in P_READ (while the RAMs read), in steps 11..13 and 37
//   (between a read and the next one from it) and in the reflection's steps between its
//   reads. Steps without a read still set rd_addr from this sum for the next read (7: the
//   vertices less 2, 17: the next face, 45: a texture record's frames).
//   rv_sum = rv_base + rv_val: a base plus the value just read. It feeds the registers that
//   hold addresses in the mesh, and rd_addr when the next read is at a corner (19), a
//   material (28), a texture record (32) or a texture coordinate (36). The cache's read
//   address never takes it.
//
    reg  [25:0] ra_base, rv_base, rv_val;
    reg  [18:0] ra_off;
    // operands of the two adders in the current step (the comment on a step names the address)
always @* begin
    ra_base = rd_addr;
    ra_off = 19'd2;                             // 8, 9, 10, 18, 20, 21, 38: the next halfword
    case(pc)
    P_READ, 6'd11, 6'd12, 6'd13, 6'd15, 6'd37, 6'd47, 6'd49, R_CNT, R_N2, R_C:
           ra_off = 19'd0;
    6'd3:  begin ra_base = blk; ra_off = 19'h10; end            // the vertex count
    6'd4:  ra_off = 19'h8;                                      // blk + 0x18
    R_FLAGS, 6'd5, R_NRM:                                       // blk + 0x1C, 0x20, 0x24
           ra_off = 19'h4;
    R_START: begin ra_base = nrm; ra_off = 19'd0; end           // the normals' count
    R_V, R_N0, R_N1: ra_off = 19'h4;                            // the normal's x, y, z
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
    6'd46: ra_off = {1'b0, fmul[31:16], 2'd0};                  // texrec + 8 + 4 frame
    default: ;
    endcase
    case(pc)
    6'd5, 6'd6, R_NRM: rv_base = obj;
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
    // a material in one colour: routine 128, and 192 (original: STARTL.EXE 0xcae84)
    wire        untex = rd_word == 32'd128 || rd_word == 32'd192;
    // Step 29 of a face in one colour reads face + 2. The choice depends on the value just
    // read, so it selects between two sums and not between the adder's inputs.
    wire [25:0] rd_a = pc == 6'd29 && untex ? face + 26'd2 : ra_sum;
    wire        rv_ld = pc == 6'd19 || pc == 6'd28 || pc == 6'd32 || pc == 6'd36;

    // A read: the address is rd_a (taken into rd_addr below), and step next takes the value.
    task read(input [5:0] next);
        begin
            ret <= next;
            pc <= P_READ;
        end
    endtask

//
// A corner goes into poly_c (the j-th corner, j < 4):
//   textured face       s = u x width (step 40, first clock), then t = v x height together
//                       with the vertex (third clock)
//   face in one colour  s = 0, t = 0 and the vertex (step 42)
//   routine 160         the vertex's reflection u, v in whole texels (step 42)
// |u|, |v| <= 2^15 and the size <= 256, so the products fit in 24 bits and the clamp of
// geom.c never acts.
//
    wire        c_s = (pc == 6'd40 && ms[1:0] == 2'd0) || pc == 6'd42;
    wire        c_tv = (pc == 6'd40 && ms[1:0] == 2'd2) || pc == 6'd42;
    wire [23:0] c_st = textured ? mul_p[23:0] : 24'd0;
    wire        c_r = refl && pc == 6'd42;
    wire [23:0] c_sv = c_r ? {{6{r_q[9]}}, r_q[9:0], 8'd0} : c_st;
    wire [23:0] c_tvv = c_r ? {{6{r_q[19]}}, r_q[19:10], 8'd0} : c_st;
    integer     k;
    // write the corner j into its slot of poly_c
always @(posedge clk) begin
    for(k = 0; k < 4; k = k + 1) if(j[1:0] == k[1:0]) begin
        if(c_s) poly_c[144 * k + 96 +: 24] <= c_sv;
        if(c_tv) begin
            poly_c[144 * k + 120 +: 24] <= c_tvv;
            poly_c[144 * k +: 96] <= v_q;
        end
    end
end

    // The accumulators, in steps 12 and 26. They start at ms 0 with the origin << 8 (step 12)
    // or 0 (the side test and the turn of a normal). The products of the operands taken at
    // ms 1..3 are added at ms 3..5.
    wire        acc_new = ms == 3'd0;
    wire        acc_go = (pc == 6'd12 || pc == 6'd26) && (ms[2] || ms == 3'd3 || acc_new);

    // The main clocked block: the read cache's state machine, the counters and the
    // accumulators, then the sequence (the case on pc, which stands still while hold is set).
always @(posedge clk) begin
    ct_we <= 0;
    // The cache: fetches the line that the waiting step needs from the SDRAM, and forgets
    // every line on request.
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
    C_FILL: if(mem_rsp_valid) begin             // cd_we writes the word into the RAM
        fill_cnt <= fill_cnt + 1'b1;
        if(fill_cnt == 3'd7) begin
            ct_we <= 1;
            ct_wa <= rd_addr[11:5];
            ct_wd <= {1'b1, rd_addr[25:12]};
            cstate <= C_SETTLE;
        end
    end
    C_SETTLE: cstate <= C_LOOK;                 // the tag is written now
    C_LOOK: cstate <= C_IDLE;                   // the line is read again, the waiting step goes on
    C_INVAL: begin
        ct_we <= 1;
        ct_wa <= inval_cnt;
        ct_wd <= 0;
        inval_cnt <= inval_cnt + 1'b1;
        if(inval_cnt == 7'd127) cstate <= C_IDLE;
    end
    default: cstate <= C_IDLE;
    endcase

    // counters and requests of the CPU's registers 20 to 22; the polygon leaves
    if(pc != P_IDLE) busy_clocks <= busy_clocks + 1'b1;
    if(wr && wr_addr == 5'd21) busy_clocks <= 0;
    if(wr && wr_addr == 5'd20) faces <= 0;
    if(wr && wr_addr == 5'd22) inval_req <= 1;
    if(cstate == C_INVAL) inval_req <= 0;
    if(poly_valid && poly_ready) poly_valid <= 0;

    if(acc_go) begin
        // One adder for each accumulator, its operands chosen: the start value plus 0, or the
        // sum plus a product.
        acc[0] <= (acc_new ? (side || nmac ? 40'sd0 : {pq0, 8'd0}) : acc[0]) +
                  (acc_new ? 40'sd0 : {{6{mul_p[33]}}, mul_p});
        acc[1] <= (acc_new ? (side || nmac ? 40'sd0 : {pq1, 8'd0}) : acc[1]) +
                  (acc_new ? 40'sd0 : {{6{mul1_p[33]}}, mul1_p});
        acc[2] <= (acc_new ? (side || nmac ? 40'sd0 : {pq2, 8'd0}) : acc[2]) +
                  (acc_new ? 40'sd0 : {{6{mul2_p[33]}}, mul2_p});
    end

    // The sequence. Every clock without hold takes the next read address.
    if(!hold) begin
    rd_check <= 0;
    rd_addr <= rv_ld ? rv_sum : rd_a;
    cd_ra <= rd_a[11:2];
    ct_ra <= rd_a[11:5];
    case(pc)
    P_IDLE: if(f_count >= 12'd19) begin         // a whole piece (19 words) is in: word 0 is read
        f_ra <= f_rp[10:0];
        f_rp <= f_rp + 1'b1;
        pk <= 0;
        pc <= P_TAKE;
    end

    P_TAKE: begin                               // word pk - 1 comes out, word pk + 1 is read
        pk <= pk + 1'b1;                        // (words 0..14 go into the small RAMs, see p_we)
        if(pk < 5'd18) begin
            f_ra <= f_rp[10:0];
            f_rp <= f_rp + 1'b1;
        end
        if(pk == 5'd16) obj <= f_q[25:0];
        if(pk == 5'd17) blk <= f_q[25:0];
        if(pk == 5'd18) begin
            row <= {f_q[4], f_q[2:0]};
            mirror <= f_q[3];
            phase <= f_q[29:16];
        end
        if(pk == 5'd19) begin
            rphase <= f_q[15:0];
            offx <= f_q[23:16];
            offz <= f_q[31:24];
            pc <= 6'd3;
        end
    end

    P_READ: begin                               // the RAMs read: the next step takes the value
        rd_check <= 1;
        pc <= ret;
    end

    P_DONE: pc <= P_IDLE;

    // The block header. Each step takes the value read before and reads the next one; the
    // comment names the address of the read the step issues.
    6'd3: read(6'd4);                           // blk + 0x10: the vertex count
    6'd4: begin
        nvert <= rd_word[9:0];
        nbad <= $signed(rd_word) < 0 || $signed(rd_word) > 512;
        read(R_FLAGS);                          // blk + 0x18: the flags
    end
    R_FLAGS: begin refl_on <= rd_word[13]; read(6'd5); end  // flag 0x2000; reads blk + 0x1C
    6'd5: begin verts <= rv_sum; read(R_NRM); end           // obj + word; reads blk + 0x20
    R_NRM: begin nrm <= rv_sum; read(6'd6); end             // obj + word; reads blk + 0x24
    6'd6: begin uvs <= rv_sum; read(6'd7); end              // obj + word; reads the face row's
                                                            // offset at blk + 0x28 + 4 row
    6'd7: begin                                 // rd_addr is set to verts - 2 for the loop below
        ep <= rv_sum;                           // blk + the face row's offset
        vi <= 0;
        pc <= nbad || $signed(rd_word) <= 0 ? P_DONE : 6'd8;
    end

    // The vertices into view space (original: STARTL.EXE 0x83380) (docs/original_geometry.md 5).
    // Steps 8 to 11 read x, y and z, step 12 multiplies, step 13 stores.
    6'd8: if(vi == nvert) begin                 // done: then the reflections, or the faces
              nf <= 0;
              vi <= 0;
              pc <= refl_on ? R_START : 6'd16;
          end else read(6'd9);                  // verts + 6 vi: x
    6'd9:  read(6'd10);                         // x into vc; reads y
    6'd10: read(6'd11);                         // y into vc; reads z
    6'd11: begin ms <= 0; nmac <= 0; pc <= 6'd12; end   // z into vc
    6'd12: begin                                // the three rows at once: column ms - 1 of each
        ms <= ms + 1'b1;
        mul_a <= {{2{vq[15]}}, vq};
        mul_b <= pq0[17:0];
        if(ms == 3'd5) pc <= nmac ? R_C : 6'd13;
    end
    6'd13: begin                                // t + (s >> 8) into the vertex RAM (t: origin)
        vi <= vi + 1'b1;
        pc <= 6'd8;
    end

    // The reflections of the vertices with a normal (geom.h). Per vertex: R_V, R_N0 to R_N2
    // read the normal, step 12 turns it into view space, R_C computes u and v.
    R_START: read(R_CNT);                       // nrm: the count of normals
    R_CNT: begin                                // ncnt = the count, limited to 0..nvert
        ncnt <= $signed(rd_word) < 0 ? 10'd0 :
                $signed(rd_word) < $signed({22'd0, nvert}) ? rd_word[9:0] : nvert;
        pc <= R_V;
    end
    R_V: if(vi == ncnt) pc <= 6'd16;            // all done: on to the faces (nf is 0)
         else begin
             v_ra <= vi[8:0];                   // reads the vertex p from the vertex RAM
             read(R_N0);                        // nrm + 4 + 12 vi: the normal's x
         end
    R_N0: read(R_N1);                           // x into vc, as Q14; reads y
    R_N1: read(R_N2);                           // y into vc; reads z
    R_N2: begin ms <= 0; nmac <= 1; rs <= 0; pc <= 6'd12; end   // z into vc; step 12 turns n
    R_C: begin                                  // see the reflection's datapath above
        rs <= rs + 1'b1;
        case(rs)
        4'd0: begin mul_a <= {pn0[16], pn0}; mul_b <= nv0; end
        4'd1: mul_b <= {pn0[16], pn0};
        4'd4: begin mul_a <= {1'b0, qx}; mul_b <= {2'd0, y0}; end
        4'd6: mul_a <= yq;                      // t1 = (x y0) >> 16
        4'd8: begin mul_a <= {2'd0, y0}; mul_b <= t3c[17:0]; end
        4'd12: begin                            // r_we writes u and v into the reflection RAM
            vi <= vi + 1'b1;
            pc <= R_V;
        end
        default: ;
        endcase
    end

    // The face row (original: STARTL.EXE 0x83468) (docs/original_geometry.md 6.1): the faces
    // of the block that can face the eye, ended by 0xFFFF; at most 1023 faces.
    6'd16: if(nf == 10'd1023) pc <= P_DONE;
           else read(6'd17);                    // ep: the next entry of the face row
    6'd17: begin
        if(rd_u16 == 32'hFFFF) pc <= P_DONE;
        else begin
            face <= rv_sum;                     // blk + ..
            ep <= ra_sum;                       // ep + 2
            nf <= nf + 1'b1;
            pc <= 6'd14;
        end
    end
    // A read whose address comes from the value just read waits a clock: the RAM's output
    // through an adder into its own address register would not make the clock.
    6'd14: read(6'd18);                         // face + 6: the number of corners
    6'd18: begin
        nc <= rd_half[2:0];
        if(rd_s16 < 3 || rd_s16 > 4) pc <= 6'd16;   // only triangles and quads are handled
        else read(6'd19);                       // face + 8: the first corner's vertex offset
    end
    6'd19: pc <= 6'd15;                         // rd_addr is set to verts + the corner's offset
    6'd15: read(6'd20);
    6'd20: read(6'd21);                         // x into vc
    6'd21: read(6'd22);                         // y into vc
    6'd22: read(6'd23);                         // z into vc; reads face + 0x14: the normal
    6'd23: read(6'd24);                         // face + 0x18 (the normal's x into nm)
    6'd24: read(6'd25);                         // face + 0x1C (y into nm)
    6'd25: begin ms <= 0; pc <= 6'd26; end      // z into nm
    6'd26: begin                                // the side test: ((c0 << 8) - e) . n
        ms <= ms + 1'b1;
        if(ms == 3'd5) pc <= 6'd27;
    end
    6'd27: if(dsum[49] ^ mirror) pc <= 6'd16;   // turned away
           else read(6'd28);                    // face + 4: the material offset

    // The material (docs/original_geometry.md 8). Step 28 sets rd_addr to the material
    // mat = mlb + offset, step 47 reads its kind.
    6'd28: pc <= 6'd47;                         // rd_addr: mat = mlb + the offset
    6'd47: read(6'd29);                         // mat: the material kind
    6'd29: begin
        blend <= rd_word == 32'd224;            // see-through through MAPn.SMG
        refl <= rd_word == 32'd160;
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
    6'd32: pc <= 6'd49;                         // rd_addr: texrec = mlb + the texture record's offset
    6'd49: read(6'd45);                         // texrec: the number of frames
    6'd45: begin                                // the frame: frames (if > 0, low half) x phase >> 14
        fmul <= rd_word[31] ? 32'd0 :
                {16'd0, rd_word[15:0]} * {16'd0, refl ? rphase : {phase, 2'd0}};  // 160: >> 16
        pc <= 6'd46;                            // rd_addr: texrec + 8
    end
    6'd46: read(6'd34);                         // texrec + 8 + 4 frame: the frame's data offset
    6'd33: begin colour <= rd_half[7:0]; j <= 0; okv <= 1; pc <= 6'd35; end
    6'd34: begin texdata <= rv_sum; colour <= 0; j <= 0; okv <= 1; pc <= 6'd35; end

    // The corners. For each corner: its vertex offset vo (the byte offset of the vertex in
    // the block's vertex array), the vertex number, and the texture coordinates.
    6'd35: if(j == 3'd0 && poly_valid) ;        // wait: the setup unit has not taken the last polygon
           else if(j == nc) pc <= 6'd44;
           else read(6'd36);                    // face + 8 + 2 j: the corner's vertex offset
    6'd36: begin                                // vertex number: vo / 6 = (vo / 2) * 43691 >> 17
        mul_a <= {3'd0, rd_half[15:1]};         // rd_addr: uvs + vo
        mul_b <= 18'sd43691;
        ms <= 0;
        pc <= 6'd37;
    end
    6'd37: begin
        ms <= ms + 1'b1;
        if(ms == 3'd2) begin
            v_ra <= mul_p[25:17];
            if(mul_p[30:17] >= {4'd0, nvert}) okv <= 0;
            if(textured && !refl) read(6'd38);  // uvs + vo
            else pc <= 6'd41;
        end
    end
    6'd38: begin mul_a <= rd_s16[17:0]; mul_b <= {9'd0, tw}; read(6'd39); end   // u x width
    6'd39: begin mul_a <= rd_s16[17:0]; mul_b <= {9'd0, th}; ms <= 0; pc <= 6'd40; end // v x height
    6'd40: begin                                // u x width (operands of 38), then v x height (39)
        ms <= ms + 1'b1;
        if(ms == 3'd2) begin                    // c_s acts at ms 0 and c_tv at ms 2: into poly_c
            j <= j + 1'b1;
            pc <= 6'd35;
        end
    end
    6'd41: pc <= 6'd42;                         // one colour or 160: the vertex comes out of the RAM
    6'd42: begin                                // c_s and c_tv put the corner into poly_c
        j <= j + 1'b1;
        pc <= 6'd35;
    end

    // The polygon goes to the setup unit (it is dropped if a corner's vertex number was too big).
    6'd44: begin
        if(okv) begin
            poly_valid <= 1;
            poly_n <= nc;
            poly_mode <= {textured, buffer, colour};
            poly_tex <= {6'd0, texdata};
            poly_size <= {cut, blend, refl, 4'd0, th, 7'd0, tw};
            faces <= faces + 1'b1;
        end
        pc <= 6'd16;
    end

    default: pc <= P_IDLE;
    endcase
    end

`ifdef GEOM_STATS
    // For the simulation only: where the clocks go, printed when the CPU clears busy_clocks.
    if(wr && wr_addr == 5'd21) begin
        $display("gu: busy %0d, reading %0d (missing %0d), waiting for setup %0d, pieces %0d, vertices %0d, reflections %0d, faces %0d, turned away %0d, handed on %0d",
                 busy_clocks, st_read, st_miss, st_setup, st_pieces, st_verts, st_refl, st_faces,
                 st_away, faces);
        st_read <= 0; st_miss <= 0; st_setup <= 0; st_pieces <= 0; st_verts <= 0; st_faces <= 0;
        st_away <= 0; st_refl <= 0;
    end else begin
        if(pc == P_READ || hold) st_read <= st_read + 1;
        if(cstate == C_MISS || cstate == C_FILL) st_miss <= st_miss + 1;
        if(pc == 6'd35 && j == 3'd0 && poly_valid) st_setup <= st_setup + 1;
        if(pc == P_TAKE && pk == 5'd19) st_pieces <= st_pieces + 1;
        if(r_we) st_refl <= st_refl + 1;
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
