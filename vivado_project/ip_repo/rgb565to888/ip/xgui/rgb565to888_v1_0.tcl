# Definitional proc to organize widgets for parameters.
proc init_gui { IPINST } {
  ipgui::add_param $IPINST -name "Component_Name"
  #Adding Page
  set Page_0 [ipgui::add_page $IPINST -name "Page 0"]
  ipgui::add_param $IPINST -name "RGB_SET" -parent ${Page_0}


}

proc update_PARAM_VALUE.RGB_SET { PARAM_VALUE.RGB_SET } {
	# Procedure called to update RGB_SET when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.RGB_SET { PARAM_VALUE.RGB_SET } {
	# Procedure called to validate RGB_SET
	return true
}


proc update_MODELPARAM_VALUE.RGB_SET { MODELPARAM_VALUE.RGB_SET PARAM_VALUE.RGB_SET } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.RGB_SET}] ${MODELPARAM_VALUE.RGB_SET}
}

