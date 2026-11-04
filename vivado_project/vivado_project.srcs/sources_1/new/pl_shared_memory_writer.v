`timescale 1ns / 1ps

// Polls the shared-memory command word and writes a 1..1024 test sequence.
// The command's low 16 bits are a 32-bit-word offset from SHMEM_BASE.
module pl_shared_memory_writer #(
    parameter [31:0] SHMEM_BASE = 32'h1800_0000,
    parameter [10:0] WORD_COUNT = 11'd1024
)(
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK",
       X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aclk, ASSOCIATED_BUSIF M_AXI, ASSOCIATED_RESET aresetn, FREQ_HZ 100000000" *)
    input wire aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST",
       X_INTERFACE_PARAMETER = "XIL_INTERFACENAME aresetn, POLARITY ACTIVE_LOW" *)
    input wire aresetn,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWADDR",
       X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXI, PROTOCOL AXI4, ADDR_WIDTH 32, DATA_WIDTH 32, ID_WIDTH 1, FREQ_HZ 100000000, READ_WRITE_MODE READ_WRITE, HAS_BURST 1, HAS_LOCK 0, HAS_CACHE 1, HAS_PROT 1, HAS_QOS 1, HAS_REGION 0, SUPPORTS_NARROW_BURST 0, AWUSER_WIDTH 0, ARUSER_WIDTH 0, WUSER_WIDTH 0, RUSER_WIDTH 0, BUSER_WIDTH 0" *)
    output reg [31:0] M_AXI_AWADDR,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWLEN" *)
    output wire [7:0] M_AXI_AWLEN,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWSIZE" *)
    output wire [2:0] M_AXI_AWSIZE,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWBURST" *)
    output wire [1:0] M_AXI_AWBURST,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWLOCK" *)
    output wire M_AXI_AWLOCK,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWCACHE" *)
    output wire [3:0] M_AXI_AWCACHE,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWPROT" *)
    output wire [2:0] M_AXI_AWPROT,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWQOS" *)
    output wire [3:0] M_AXI_AWQOS,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWID" *)
    output wire [0:0] M_AXI_AWID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWVALID" *)
    output wire M_AXI_AWVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI AWREADY" *)
    input wire M_AXI_AWREADY,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI WDATA" *)
    output wire [31:0] M_AXI_WDATA,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI WSTRB" *)
    output wire [3:0] M_AXI_WSTRB,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI WLAST" *)
    output wire M_AXI_WLAST,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI WVALID" *)
    output wire M_AXI_WVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI WREADY" *)
    input wire M_AXI_WREADY,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI BRESP" *)
    input wire [1:0] M_AXI_BRESP,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI BID" *)
    input wire [0:0] M_AXI_BID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI BVALID" *)
    input wire M_AXI_BVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI BREADY" *)
    output wire M_AXI_BREADY,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARADDR" *)
    output wire [31:0] M_AXI_ARADDR,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARLEN" *)
    output wire [7:0] M_AXI_ARLEN,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARSIZE" *)
    output wire [2:0] M_AXI_ARSIZE,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARBURST" *)
    output wire [1:0] M_AXI_ARBURST,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARLOCK" *)
    output wire M_AXI_ARLOCK,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARCACHE" *)
    output wire [3:0] M_AXI_ARCACHE,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARPROT" *)
    output wire [2:0] M_AXI_ARPROT,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARQOS" *)
    output wire [3:0] M_AXI_ARQOS,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARID" *)
    output wire [0:0] M_AXI_ARID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARVALID" *)
    output wire M_AXI_ARVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI ARREADY" *)
    input wire M_AXI_ARREADY,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI RDATA" *)
    input wire [31:0] M_AXI_RDATA,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI RRESP" *)
    input wire [1:0] M_AXI_RRESP,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI RLAST" *)
    input wire M_AXI_RLAST,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI RID" *)
    input wire [0:0] M_AXI_RID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI RVALID" *)
    input wire M_AXI_RVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 M_AXI RREADY" *)
    output wire M_AXI_RREADY
);

    localparam [2:0] ST_POLL_AR   = 3'd0,
                     ST_POLL_R    = 3'd1,
                     ST_WRITE_AW  = 3'd2,
                     ST_WRITE_W   = 3'd3,
                     ST_WRITE_B   = 3'd4,
                     ST_CLEAR_AR  = 3'd5,
                     ST_CLEAR_R   = 3'd6;

    reg [2:0] state;
    reg [31:0] write_address;
    reg [10:0] word_index;

    assign M_AXI_AWLEN   = 8'd0;
    assign M_AXI_AWSIZE  = 3'd2;
    assign M_AXI_AWBURST = 2'b01;
    assign M_AXI_AWLOCK  = 1'b0;
    assign M_AXI_AWCACHE = 4'b0011;
    assign M_AXI_AWPROT  = 3'b000;
    assign M_AXI_AWQOS   = 4'b0000;
    assign M_AXI_AWID    = 1'b0;
    assign M_AXI_AWVALID = (state == ST_WRITE_AW);

    assign M_AXI_WDATA   = {21'd0, word_index} + 32'd1;
    assign M_AXI_WSTRB   = 4'b1111;
    assign M_AXI_WLAST   = 1'b1;
    assign M_AXI_WVALID  = (state == ST_WRITE_W);
    assign M_AXI_BREADY  = (state == ST_WRITE_B);

    assign M_AXI_ARADDR  = SHMEM_BASE;
    assign M_AXI_ARLEN   = 8'd0;
    assign M_AXI_ARSIZE  = 3'd2;
    assign M_AXI_ARBURST = 2'b01;
    assign M_AXI_ARLOCK  = 1'b0;
    assign M_AXI_ARCACHE = 4'b0011;
    assign M_AXI_ARPROT  = 3'b000;
    assign M_AXI_ARQOS   = 4'b0000;
    assign M_AXI_ARID    = 1'b0;
    assign M_AXI_ARVALID = (state == ST_POLL_AR) || (state == ST_CLEAR_AR);
    assign M_AXI_RREADY  = (state == ST_POLL_R) || (state == ST_CLEAR_R);

    always @(posedge aclk) begin
        if (!aresetn) begin
            state         <= ST_POLL_AR;
            write_address <= SHMEM_BASE;
            word_index    <= 11'd0;
            M_AXI_AWADDR  <= SHMEM_BASE;
        end else begin
            case (state)
                ST_POLL_AR: begin
                    if (M_AXI_ARREADY)
                        state <= ST_POLL_R;
                end

                ST_POLL_R: begin
                    if (M_AXI_RVALID) begin
                        if (M_AXI_RDATA[31:16] == 16'h55AA) begin
                            write_address <= SHMEM_BASE + {14'd0, M_AXI_RDATA[15:0], 2'b00};
                            M_AXI_AWADDR  <= SHMEM_BASE + {14'd0, M_AXI_RDATA[15:0], 2'b00};
                            word_index    <= 11'd0;
                            state         <= ST_WRITE_AW;
                        end else begin
                            state <= ST_POLL_AR;
                        end
                    end
                end

                ST_WRITE_AW: begin
                    if (M_AXI_AWREADY)
                        state <= ST_WRITE_W;
                end

                ST_WRITE_W: begin
                    if (M_AXI_WREADY)
                        state <= ST_WRITE_B;
                end

                ST_WRITE_B: begin
                    if (M_AXI_BVALID) begin
                        if (word_index == (WORD_COUNT - 1'b1)) begin
                            state <= ST_CLEAR_AR;
                        end else begin
                            word_index    <= word_index + 1'b1;
                            write_address <= write_address + 32'd4;
                            M_AXI_AWADDR  <= write_address + 32'd4;
                            state         <= ST_WRITE_AW;
                        end
                    end
                end

                // Do not replay a command that remains in the mailbox. Linux
                // can arm another transfer by changing/clearing the word first.
                ST_CLEAR_AR: begin
                    if (M_AXI_ARREADY)
                        state <= ST_CLEAR_R;
                end

                ST_CLEAR_R: begin
                    if (M_AXI_RVALID) begin
                        if (M_AXI_RDATA[31:16] != 16'h55AA)
                            state <= ST_POLL_AR;
                        else
                            state <= ST_CLEAR_AR;
                    end
                end

                default: state <= ST_POLL_AR;
            endcase
        end
    end

endmodule
