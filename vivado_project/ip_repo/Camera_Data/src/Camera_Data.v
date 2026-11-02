module Camera_Data(
	Rst_n,				//复位
	PCLK,				//像素时钟
	Vsync,				//场同步信号
	Href,				//行同步信号
	Data,				//数据
	
	DataValid,			//数据有效信号
	DataPixel,			//像素数据
	
	Cam_Rst_n,  		//cmos 复位信号，低电平有效
	Cam_Pwdn,			//电源休眠模式选择

	//RGB888接口
	Frame_Clk,		//时钟信号
	Frame_Ce,		//时钟使能信号

	Frame_Vsync		//帧有效信号
);

	input Rst_n;			//复位
	input PCLK;				//像素时钟
	input Vsync;			//场同步信号
	input Href;				//行同步信号
	input [7:0]Data;		//数据
	
	output DataValid;		//数据有效信号
	output [23:0]DataPixel;	//像素数据
	
	output Cam_Rst_n;//cmos 复位信号，低电平有效
    output Cam_Pwdn;//电源休眠模式选择

    output Frame_Clk;//时钟信号
    output Frame_Ce;//时钟使能信号

	output Frame_Vsync;//帧有效信号

	reg r_Vsync;			
	reg r_Href;
	reg [7:0]r_Data;
	
	reg [15:0]r_DataPixel;
	reg r_DataValid;
	reg [12:0]Hcount;
	reg [11:0]Vcount;
	reg [3:0]FrameCnt;
	
	reg Dump_Frame;
	
	assign DataPixel = Dump_Frame ? {r_DataPixel[15:11], 3'd0, r_DataPixel[10:5], 2'd0, r_DataPixel[4:0], 3'd0} : 24'd0;
	assign DataValid = r_Href & Dump_Frame;
	assign Frame_Vsync = r_Vsync & Dump_Frame;
	
	//摄像头硬件复位,固定高电平
	assign  Cam_Rst_n = 1'b1;

	//电源休眠模式选择 0：正常模式 1：电源休眠模式
	assign  Cam_Pwdn  = 1'b0;
	
	//摄像头时钟使能
	assign  Frame_Ce = ((Hcount[0]) || (!r_Href)) & Dump_Frame;//1'b1;//(r_DataValid & Dump_Frame)||(!r_DataValid);
	//assign  Frame_Ce = ((!Hcount[0]) || (!r_Href)) & Dump_Frame;//1'b1;//(r_DataValid & Dump_Frame)||(!r_DataValid);
	
	//时钟为像素时钟
	assign  Frame_Clk = PCLK;

	//打拍
	always@(posedge PCLK)
	begin
		r_Vsync <= Vsync;
		r_Href <= Href;
		r_Data <= Data;
	end
	
	//行同步信号为1时，行计数器加一（行同步信号为0时归零）
	always@(posedge PCLK or negedge Rst_n)
	if(!Rst_n)
		Hcount <= 0;
	else if(r_Href)
		Hcount <= Hcount + 1'd1;
	else
		Hcount <= 0;

	//8位转16位，赋予像素数据
	always@(posedge PCLK or negedge Rst_n)
	if(!Rst_n)
		r_DataPixel <= 0;
	else if(!Hcount[0])
		r_DataPixel <= {r_Data,Data};
//	else 
//		r_DataPixel[7:0] <= r_Data;
	
	//产生数据有效信号
	always@(posedge PCLK or negedge Rst_n)
	if(!Rst_n)
		r_DataValid <= 0;
	else if(Hcount[0] && r_Href)
		r_DataValid <= 1;
	else
		r_DataValid <= 0;
	
	//行同步信号由0变为1时，列计数器加一（场同步信号为1时归零）
	always@(posedge PCLK or negedge Rst_n)
	if(!Rst_n)
		Vcount <= 0;
	else if(r_Vsync)
		Vcount <= 0;
	else if({r_Href,Href} == 2'b01)
		Vcount <= Vcount + 1'd1;
	else
		Vcount <= Vcount;
	
	//场同步信号由0变为1时，帧计数加一，最大为10
	always@(posedge PCLK or negedge Rst_n)
	if(!Rst_n)	
		FrameCnt <= 0;
	else if({r_Vsync,Vsync}== 2'b01)begin
		if(FrameCnt >= 10)
			FrameCnt <= 4'd10;
		else
			FrameCnt <= FrameCnt + 1'd1;
	end
	else
		FrameCnt <= FrameCnt;
	
	//当计数大于等于10帧时，Dump_Frame变为1，否则为0
	always@(posedge PCLK or negedge Rst_n)
	if(!Rst_n)
		Dump_Frame <= 0;
	else if(FrameCnt >= 10)
		Dump_Frame <= 1'd1;
	else
		Dump_Frame <= 0;

endmodule
