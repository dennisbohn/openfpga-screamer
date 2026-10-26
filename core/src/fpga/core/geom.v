//
// Geometry unit: one piece of the track (a placed mesh) at a time, bit for bit the C model
// firmware/geom.c (geom.h has the arithmetic). The CPU hands over the piece's place in view
// space; the unit reads the mesh from the SDRAM itself (block header, vertices, the face
// row for the eye's side, faces, texture coordinates, MLB materials), takes the vertices
// into view space (block RAM, up to 512), leaves out the faces turned away and those of
// routine 224, and hands each other face to the setup unit as a polygon in fixed point.
//
// One multiplier (operands registered, the product three clocks after it was issued) and a
// read cache of 4 KB (direct-mapped, lines of 32 bytes from the SDRAM). A step of the
// sequence below either reads a value (two clocks when it hits) or works on what it read.
//
// Registers (soc.v maps them at 0xF000_3100 + 4 * wr_addr):
//   0..8    local to view: rows r, u, f of columns x, y, z, 16 fraction bits (within +-2^17)
//   9..11   the piece's origin in view space, 8 fraction bits
//   12..14  the eye in the piece's frame, 8 fraction bits
//   15, 16  SDRAM byte addresses of the mesh's object and its block
//   17      SDRAM byte address of the MLB      18  [8] the buffer drawn into
//   19      write: [2:0] the face row, starts the piece (soc.v holds the write back while busy;
//           0..18 are taken then, so the next piece can be written meanwhile)
//   20      faces handed on since the last clear (write: clear)
//   21      clocks busy since the last clear (write: clear)
//   22      write: forget the cached lines (after the mesh data changed)
//

module geom (
    input   wire            clk,
    input   wire            reset,

    input   wire            wr,
    input   wire    [4:0]   wr_addr,
    input   wire    [31:0]  wr_data,
    output  wire            busy,
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
// registers the CPU writes; taken when a piece starts
//
    reg  signed [31:0] in_m [0:8];
    reg  signed [31:0] in_t [0:2];
    reg  signed [31:0] in_e [0:2];
    reg  [25:0] in_obj, in_blk, mlb;
    reg         buffer;
    wire        go = wr && wr_addr == 5'd19 && !reset;
    reg         inval_req = 1;

    integer     ri;
always @(posedge clk) begin
    for(ri = 0; ri < 9; ri = ri + 1) if(wr && wr_addr == ri[4:0]) in_m[ri] <= wr_data;
    for(ri = 0; ri < 3; ri = ri + 1) begin
        if(wr && wr_addr == 5'd9 + ri[4:0]) in_t[ri] <= wr_data;
        if(wr && wr_addr == 5'd12 + ri[4:0]) in_e[ri] <= wr_data;
    end
    if(wr && wr_addr == 5'd15) in_obj <= wr_data[25:0];
    if(wr && wr_addr == 5'd16) in_blk <= wr_data[25:0];
    if(wr && wr_addr == 5'd17) mlb <= wr_data[25:0];
    if(wr && wr_addr == 5'd18) buffer <= wr_data[8];
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

    localparam  [2:0]   C_IDLE = 3'd0, C_LOOK = 3'd1, C_CHECK = 3'd2, C_MISS = 3'd3, C_FILL = 3'd4,
                        C_INVAL = 3'd5;
    reg  [2:0]  cstate = C_INVAL;
    reg  [25:0] rd_addr;
    reg         rd_go = 0, rd_done = 0;
    reg  [31:0] rd_word;
    reg  [2:0]  fill_cnt;
    reg  [6:0]  inval_cnt = 0;

always @(posedge clk) begin
    rd_done <= 0;
    cd_we <= 0;
    ct_we <= 0;
    case(cstate)
    C_IDLE: if(inval_req) begin
        inval_cnt <= 0;
        cstate <= C_INVAL;
    end else if(rd_go) begin
        cd_ra <= {rd_addr[11:5], rd_addr[4:2]};
        ct_ra <= rd_addr[11:5];
        cstate <= C_LOOK;
    end
    C_LOOK: cstate <= C_CHECK;                  // tag and word come out
    C_CHECK: if(ct_q == {1'b1, rd_addr[25:12]}) begin
        rd_word <= cd_q;
        rd_done <= 1;
        cstate <= C_IDLE;
    end else begin
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
        if(fill_cnt == 3'd7) begin              // the line is in: looked up again
            ct_we <= 1;
            ct_wa <= rd_addr[11:5];
            ct_wd <= {1'b1, rd_addr[25:12]};
            cd_ra <= {rd_addr[11:5], rd_addr[4:2]};
            ct_ra <= rd_addr[11:5];
            cstate <= C_LOOK;
        end
    end
    C_INVAL: begin
        ct_we <= 1;
        ct_wa <= inval_cnt;
        ct_wd <= 0;
        inval_cnt <= inval_cnt + 1'b1;
        if(inval_cnt == 7'd127) cstate <= C_IDLE;
    end
    default: cstate <= C_IDLE;
    endcase
    if(reset) begin
        mem_valid <= 0;
        cstate <= C_INVAL;
        inval_cnt <= 0;
    end
end

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
// the multiplier
//
    reg  signed [35:0] mul_a;
    reg  signed [17:0] mul_b;
    reg  signed [53:0] mul_p1, mul_p2;
always @(posedge clk) begin
    mul_p1 <= mul_a * mul_b;
    mul_p2 <= mul_p1;
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
    reg  signed [31:0] m [0:8];
    reg  signed [31:0] t [0:2];
    reg  signed [31:0] e [0:2];
    reg  [25:0] obj, blk, verts, uvs, ep, face, mat, texdata;
    reg  [2:0]  row;
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

    localparam  [5:0]   P_IDLE = 6'd0, P_READ = 6'd1, P_DONE = 6'd2;

    assign busy = pc != P_IDLE || poly_valid;

    wire [25:0] mat_now = mlb + {{10{rd_half[15]}}, rd_half};
    wire        col_x = ms == 4'd0 || ms == 4'd3 || ms == 4'd6, col_y = ms == 4'd1 || ms == 4'd4 || ms == 4'd7;
    wire signed [35:0] coord = col_x ? {{20{vx[15]}}, vx} : col_y ? {{20{vy[15]}}, vy} : {{20{vz[15]}}, vz};
    wire signed [35:0] bx = $signed({{12{vx[15]}}, vx, 8'd0}) - {{4{e[0][31]}}, e[0]};
    wire signed [35:0] by = $signed({{12{vy[15]}}, vy, 8'd0}) - {{4{e[1][31]}}, e[1]};
    wire signed [35:0] bz = $signed({{12{vz[15]}}, vz, 8'd0}) - {{4{e[2][31]}}, e[2]};

    // a read: the address, and the step that takes the value
    task read(input [25:0] a, input [5:0] next);
        begin
            rd_addr <= a;
            rd_go <= 1;
            ret <= next;
            pc <= P_READ;
        end
    endtask

    integer     k;
always @(posedge clk) begin
    v_we <= 0;
    if(pc != P_IDLE) busy_clocks <= busy_clocks + 1'b1;
    if(wr && wr_addr == 5'd21) busy_clocks <= 0;
    if(wr && wr_addr == 5'd20) faces <= 0;
    if(wr && wr_addr == 5'd22) inval_req <= 1;
    if(cstate == C_INVAL) inval_req <= 0;
    if(poly_valid && poly_ready) poly_valid <= 0;

    case(pc)
    P_IDLE: if(go) begin
        for(k = 0; k < 9; k = k + 1) m[k] <= in_m[k];
        for(k = 0; k < 3; k = k + 1) begin t[k] <= in_t[k]; e[k] <= in_e[k]; end
        obj <= in_obj;
        blk <= in_blk;
        row <= wr_data[2:0];
        pc <= 6'd3;
    end

    P_READ: begin                               // the cache took the address; then the value
        if(cstate == C_LOOK) rd_go <= 0;
        if(rd_done) pc <= ret;
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
    6'd12: begin                                // 9 products: column ms % 3 of row ms / 3
        ms <= ms + 1'b1;
        if(ms < 4'd9) begin
            mul_a <= coord;
            mul_b <= m[ms][17:0];
        end
        if(ms >= 4'd3) begin
            case(ms - 4'd3)
            4'd0: acc[0] <= mul_p2[35:0];
            4'd1, 4'd2: acc[0] <= acc[0] + mul_p2[35:0];
            4'd3: acc[1] <= mul_p2[35:0];
            4'd4, 4'd5: acc[1] <= acc[1] + mul_p2[35:0];
            4'd6: acc[2] <= mul_p2[35:0];
            default: acc[2] <= acc[2] + mul_p2[35:0];
            endcase
        end
        if(ms == 4'd11) pc <= 6'd13;
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
            read(blk + {10'd0, rd_half} + 26'h6, 6'd18);
        end
    end
    6'd18: begin
        nc <= rd_half[2:0];
        if(rd_s16 < 3 || rd_s16 > 4) pc <= 6'd16;
        else read(face + 26'h8, 6'd19);
    end
    6'd19: begin c0off <= rd_half; read(verts + {10'd0, rd_half}, 6'd20); end
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
    6'd27: if(dot[53]) pc <= 6'd16;             // turned away
           else read(face + 26'h4, 6'd28);

    // the material
    6'd28: begin mat <= mat_now; read(mat_now, 6'd29); end
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
    6'd32: read(mlb + rd_word[25:0] + 26'h8, 6'd34);
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
    6'd39: begin cs <= sat24(mul_p2); mul_a <= {{20{rd_half[15]}}, rd_half}; mul_b <= {9'd0, th}; ms <= 0; pc <= 6'd40; end
    6'd40: begin
        ms <= ms + 1'b1;
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

    if(reset) begin
        pc <= P_IDLE;
        rd_go <= 0;
        poly_valid <= 0;
    end
end

endmodule
