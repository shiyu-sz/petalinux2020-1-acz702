set_property PACKAGE_PIN F20 [get_ports {GPIO_PL_EMIO_tri_io[0]}]
set_property PACKAGE_PIN T14 [get_ports {GPIO_PL_EMIO_tri_io[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {GPIO_PL_EMIO_tri_io[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {GPIO_PL_EMIO_tri_io[1]}]

set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]

set_property PACKAGE_PIN L15 [get_ports pl_uart_rxd]
set_property PACKAGE_PIN K14 [get_ports pl_uart_txd]
set_property IOSTANDARD LVCMOS33 [get_ports pl_uart_rxd]
set_property IOSTANDARD LVCMOS33 [get_ports pl_uart_txd]
