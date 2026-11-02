module rgb888to565(
    input m_clk,
    input s_clk,
	input [23:0]s_axis_tdata,
	input s_axis_tlast,
	output s_axis_tready,
	input s_axis_tuser,
	input s_axis_tvalid,
	
	output [15:0]m_axis_tdata,
	output m_axis_tlast,
	input m_axis_tready,
	output m_axis_tuser,
	output m_axis_tvalid
);
parameter RGB_SET = 1;
assign m_axis_tlast = s_axis_tlast;
assign s_axis_tready = m_axis_tready;
assign m_axis_tuser = s_axis_tuser;
assign m_axis_tvalid = s_axis_tvalid;
assign m_axis_tdata = RGB_SET?{s_axis_tdata[23:19],s_axis_tdata[15:10],s_axis_tdata[7:3]}:{s_axis_tdata[12:10],s_axis_tdata[7:3], s_axis_tdata[23:19],s_axis_tdata[15:13]};
//7:0 15:8      12:10+7:3 23:19+15:13  
endmodule
