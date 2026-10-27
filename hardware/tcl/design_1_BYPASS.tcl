################################################################
# BYPASS overlay —— 去除 bpm_detect，验证基础采集链路
# 数据通路: adau1761.M_AXIS -> axis_data_fifo -> segment -> axi_dma S2MM -> DDR
# 用途: 隔离定位。若此版 DMA 能收到非零样本 => adau+FIFO+segment+DMA+软件全通,
#      问题锁定在 bpm_detect 的集成/驱动; 若还全零 => 问题在 adau 握手或 FIFO。
# 由 design_1.tcl (XSA31) 派生: 删除 bpm_detect, segment.M_AXIS 直连 DMA。
#
# 用法: 在 Vivado 中 source 本文件 或 新建 project 后 source。
#   source F:/小艺工作空间/BPM_DETECT_JUMP/design_1_BYPASS.tcl
################################################################
namespace eval _tcl {
proc get_script_folder {} {
   set script_path [file normalize [info script]]
   set script_folder [file dirname $script_path]
   return $script_folder
}
}
variable script_folder
set script_folder [_tcl::get_script_folder]

set scripts_vivado_version 2024.2
set current_vivado_version [version -short]

if { [string first $scripts_vivado_version $current_vivado_version] == -1 } {
   if { [string compare $scripts_vivado_version $current_vivado_version] > 0 } {
      catch {common::send_gid_msg -ssname BD::TCL -id 2042 -severity "ERROR" "Generated with Vivado <$scripts_vivado_version>, running $current_vivado_version. Create a project in the same version."}
   } else {
      catch {common::send_gid_msg -ssname BD::TCL -id 2041 -severity "ERROR" "Generated with Vivado <$scripts_vivado_version> < $current_vivado_version. Upgrade then re-run write_bd_tcl."}
   }
   return 1
}

set list_projs [get_projects -quiet]
if { $list_projs eq "" } {
   create_project project_1 myproj -part xc7z020clg400-1
   set_property BOARD_PART tul.com.tw:pynq-z2:part0:1.0 [current_project]
}

variable design_name
set design_name design_1_bypass

set cur_design [current_bd_design -quiet]
set list_cells [get_bd_cells -quiet]

if { ${design_name} eq "" } {
   set errMsg "Please set <design_name>."
   set nRet 1
} elseif { ${cur_design} ne "" && ${list_cells} eq "" } {
   set design_name [get_property NAME $cur_design]
} elseif { ${cur_design} ne "" && $list_cells ne "" && $cur_design eq $design_name } {
   set errMsg "Design <$design_name> already exists in your project, please set <design_name> to another value."
   set nRet 1
} elseif { [get_files -quiet ${design_name}.bd] ne "" } {
   set errMsg "Design <$design_name> already exists, set <design_name> to another value."
   set nRet 2
} else {
   create_bd_design $design_name
   current_bd_design $design_name
}

proc create_root_design { parentCell } {
  variable script_folder
  variable design_name

  if { $parentCell eq "" } {
     set parentCell [get_bd_cells /]
  }
  set parentObj [get_bd_cells $parentCell]
  if { $parentObj == "" } { return }
  set parentType [get_property TYPE $parentObj]
  if { $parentType ne "hier" } { return }
  set oldCurInst [current_bd_instance .]
  current_bd_instance $parentObj

  # Create interface ports
  set DDR_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:ddrx_rtl:1.0 DDR_0 ]
  set FIXED_IO_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:display_processing_system7:fixedio_rtl:1.0 FIXED_IO_0 ]
  set IIC_0_0 [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 IIC_0_0 ]

  # Create ports
  set BCLK_0 [ create_bd_port -dir O BCLK_0 ]
  set LRCLK_0 [ create_bd_port -dir O LRCLK_0 ]
  set SDATA_O_0 [ create_bd_port -dir O SDATA_O_0 ]
  set SDATA_I_0 [ create_bd_port -dir I SDATA_I_0 ]
  set MCLK_0 [ create_bd_port -dir O -type clk MCLK_0 ]
  set RESTN_0 [ create_bd_port -dir O -from 0 -to 0 -type rst RESTN_0 ]

  # Create instance: processing_system7_0
  set processing_system7_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 processing_system7_0 ]
  set_property -dict [list \
    CONFIG.PCW_ACT_APU_PERIPHERAL_FREQMHZ {666.666687} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_FPGA_FCLK0_ENABLE {1} \
    CONFIG.PCW_CLK0_FREQ {100000000} \
    CONFIG.PCW_EN_EMIO_I2C0 {1} \
    CONFIG.PCW_EN_I2C0 {1} \
    CONFIG.PCW_I2C0_I2C0_IO {EMIO} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
    CONFIG.PCW_USE_DMA0 {1} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_DDR_RAM_HIGHADDR {0x1FFFFFFF} \
  ] $processing_system7_0

  # Create instance: axi_dma_0 (S2MM only)
  set axi_dma_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma_0 ]
  set_property -dict [list \
    CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_include_sg {0} \
    CONFIG.c_sg_length_width {26} \
  ] $axi_dma_0

  # Create instance: axi_interconnect_0 (3 slaves: adau, dma, segment)
  set axi_interconnect_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_interconnect_0 ]
  set_property CONFIG.NUM_MI {3} $axi_interconnect_0

  # Create instance: adau1761_0
  set adau1761_0 [ create_bd_cell -type ip -vlnv xilinx.com:user:adau1761:1.0 adau1761_0 ]
  set_property -dict [list \
    CONFIG.C_M_AXIS_START_COUNT {32} \
    CONFIG.C_M_AXIS_TDATA_WIDTH {32} \
    CONFIG.C_S_AXIS_TDATA_WIDTH {32} \
    CONFIG.C_S_AXI_DATA_WIDTH {32} \
  ] $adau1761_0

  # Create instance: proc_sys_reset_0
  set proc_sys_reset_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_0 ]

  # Create instance: xlconcat_0 (1 interrupt: dma s2mm)
  set xlconcat_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_0 ]
  set_property CONFIG.NUM_PORTS {1} $xlconcat_0

  # Create instance: axi_mem_intercon
  set axi_mem_intercon [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_mem_intercon ]
  set_property CONFIG.NUM_MI {1} $axi_mem_intercon

  # Create instance: xlconstant_tvalid
  set xlconstant_tvalid [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_tvalid ]
  set_property CONFIG.CONST_VAL {1} $xlconstant_tvalid
  set_property CONFIG.CONST_WIDTH {1} $xlconstant_tvalid

  # Create instance: segment_stream_0
  set segment_stream_0 [ create_bd_cell -type ip -vlnv xilinx.com:user:segment_stream:1.0 segment_stream_0 ]

  # Create instance: clk_wiz_0 (MCLK 25MHz)
  set clk_wiz_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz:6.0 clk_wiz_0 ]
  set_property -dict [list \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {25} \
    CONFIG.CLKOUT1_JITTER {181.828} \
    CONFIG.CLKOUT1_PHASE_ERROR {104.359} \
    CONFIG.MMCM_CLKFBOUT_MULT_F {9.125} \
    CONFIG.MMCM_CLKIN1_PERIOD {10.000} \
    CONFIG.MMCM_CLKOUT0_DIVIDE_F {36.500} \
    CONFIG.MMCM_DIVCLK_DIVIDE {1} \
    CONFIG.RESET_PORT {resetn} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
  ] $clk_wiz_0

  # Create instance: xlconstant_0
  set xlconstant_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_0 ]

  # Create instance: xlconstant_1
  set xlconstant_1 [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_1 ]

  # Create instance: axis_data_fifo_0
  set axis_data_fifo_0 [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo:2.0 axis_data_fifo_0 ]

  # ==== Interface connections ====
  connect_bd_intf_net -intf_net adau1761_0_M_AXIS [get_bd_intf_pins adau1761_0/M_AXIS] [get_bd_intf_pins axis_data_fifo_0/S_AXIS]
  connect_bd_intf_net -intf_net axi_dma_0_M_AXI_S2MM [get_bd_intf_pins axi_dma_0/M_AXI_S2MM] [get_bd_intf_pins axi_mem_intercon/S00_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_0_M00_AXI [get_bd_intf_pins axi_interconnect_0/M00_AXI] [get_bd_intf_pins adau1761_0/S_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_0_M01_AXI [get_bd_intf_pins axi_dma_0/S_AXI_LITE] [get_bd_intf_pins axi_interconnect_0/M01_AXI]
  connect_bd_intf_net -intf_net axi_interconnect_0_M02_AXI [get_bd_intf_pins axi_interconnect_0/M02_AXI] [get_bd_intf_pins segment_stream_0/S_AXI]
  connect_bd_intf_net -intf_net axi_mem_intercon_M00_AXI [get_bd_intf_pins axi_mem_intercon/M00_AXI] [get_bd_intf_pins processing_system7_0/S_AXI_HP0]
  connect_bd_intf_net -intf_net axis_data_fifo_0_M_AXIS [get_bd_intf_pins axis_data_fifo_0/M_AXIS] [get_bd_intf_pins segment_stream_0/S_AXIS]
  connect_bd_intf_net -intf_net processing_system7_0_DDR [get_bd_intf_ports DDR_0] [get_bd_intf_pins processing_system7_0/DDR]
  connect_bd_intf_net -intf_net processing_system7_0_FIXED_IO [get_bd_intf_ports FIXED_IO_0] [get_bd_intf_pins processing_system7_0/FIXED_IO]
  connect_bd_intf_net -intf_net processing_system7_0_IIC_0 [get_bd_intf_ports IIC_0_0] [get_bd_intf_pins processing_system7_0/IIC_0]
  connect_bd_intf_net -intf_net processing_system7_0_M_AXI_GP0 [get_bd_intf_pins processing_system7_0/M_AXI_GP0] [get_bd_intf_pins axi_interconnect_0/S00_AXI]
  # Bypass: segment -> DMA directly (original BD: segment -> bpm.audio_in -> bpm.audio_out -> DMA)
  connect_bd_intf_net -intf_net segment_stream_0_M_AXIS [get_bd_intf_pins segment_stream_0/M_AXIS] [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]

  # ==== Port connections ====
  connect_bd_net -net SDATA_I_0_1 [get_bd_ports SDATA_I_0] [get_bd_pins adau1761_0/SDATA_I]
  connect_bd_net -net adau1761_0_BCLK [get_bd_pins adau1761_0/BCLK] [get_bd_ports BCLK_0]
  connect_bd_net -net adau1761_0_LRCLK [get_bd_pins adau1761_0/LRCLK] [get_bd_ports LRCLK_0]
  connect_bd_net -net adau1761_0_SDATA_O [get_bd_pins adau1761_0/SDATA_O] [get_bd_ports SDATA_O_0]
  connect_bd_net -net axi_dma_0_s2mm_introut [get_bd_pins axi_dma_0/s2mm_introut] [get_bd_pins xlconcat_0/In0]
  connect_bd_net -net clk_wiz_0_clk_out1 [get_bd_pins clk_wiz_0/clk_out1] [get_bd_ports MCLK_0]
  connect_bd_net -net clk_wiz_0_locked [get_bd_pins clk_wiz_0/locked] [get_bd_pins proc_sys_reset_0/dcm_locked]
  connect_bd_net -net proc_sys_reset_0_peripheral_aresetn [get_bd_pins proc_sys_reset_0/peripheral_aresetn] \
    [get_bd_pins axi_dma_0/axi_resetn] \
    [get_bd_pins axi_interconnect_0/M02_ARESETN] \
    [get_bd_pins axi_interconnect_0/M01_ARESETN] \
    [get_bd_pins axi_interconnect_0/M00_ARESETN] \
    [get_bd_pins axi_interconnect_0/S00_ARESETN] \
    [get_bd_pins axi_interconnect_0/ARESETN] \
    [get_bd_pins axi_mem_intercon/S00_ARESETN] \
    [get_bd_pins axi_mem_intercon/M00_ARESETN] \
    [get_bd_pins axi_mem_intercon/ARESETN] \
    [get_bd_pins segment_stream_0/aresetn] \
    [get_bd_pins adau1761_0/s_axi_aresetn] \
    [get_bd_pins axis_data_fifo_0/s_axis_aresetn]
  connect_bd_net -net processing_system7_0_FCLK_CLK0 [get_bd_pins processing_system7_0/FCLK_CLK0] \
    [get_bd_pins proc_sys_reset_0/slowest_sync_clk] \
    [get_bd_pins axi_dma_0/s_axi_lite_aclk] \
    [get_bd_pins axi_dma_0/m_axi_s2mm_aclk] \
    [get_bd_pins axi_interconnect_0/ACLK] \
    [get_bd_pins axi_interconnect_0/S00_ACLK] \
    [get_bd_pins axi_interconnect_0/M00_ACLK] \
    [get_bd_pins axi_interconnect_0/M01_ACLK] \
    [get_bd_pins axi_interconnect_0/M02_ACLK] \
    [get_bd_pins processing_system7_0/M_AXI_GP0_ACLK] \
    [get_bd_pins axi_mem_intercon/S00_ACLK] \
    [get_bd_pins processing_system7_0/S_AXI_HP0_ACLK] \
    [get_bd_pins axi_mem_intercon/M00_ACLK] \
    [get_bd_pins axi_mem_intercon/ACLK] \
    [get_bd_pins processing_system7_0/DMA0_ACLK] \
    [get_bd_pins segment_stream_0/aclk] \
    [get_bd_pins clk_wiz_0/clk_in1] \
    [get_bd_pins adau1761_0/s_axi_aclk] \
    [get_bd_pins axis_data_fifo_0/s_axis_aclk]
  connect_bd_net -net processing_system7_0_FCLK_RESET0_N [get_bd_pins processing_system7_0/FCLK_RESET0_N] [get_bd_pins proc_sys_reset_0/ext_reset_in]
  connect_bd_net -net xlconcat_0_dout [get_bd_pins xlconcat_0/dout] [get_bd_pins processing_system7_0/IRQ_F2P]
  connect_bd_net -net xlconstant_0_dout [get_bd_pins xlconstant_0/dout] [get_bd_ports RESTN_0]
  connect_bd_net -net xlconstant_1_dout [get_bd_pins xlconstant_1/dout] [get_bd_pins clk_wiz_0/resetn]
  connect_bd_net -net xlconstant_tvalid_dout [get_bd_pins xlconstant_tvalid/dout] [get_bd_pins adau1761_0/s_axis_tvalid]

  # Create address segments (3 masters addressable by PS)
  assign_bd_address -offset 0x44A00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces processing_system7_0/Data] [get_bd_addr_segs adau1761_0/S_AXI/S_AXI_reg] -force
  assign_bd_address -offset 0x41E00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces processing_system7_0/Data] [get_bd_addr_segs axi_dma_0/S_AXI_LITE/Reg] -force
  assign_bd_address -offset 0x43C00000 -range 0x00010000 -target_address_space [get_bd_addr_spaces processing_system7_0/Data] [get_bd_addr_segs segment_stream_0/S_AXI/S_AXI_reg] -force
  # DMA S2MM data space -> DDR
  assign_bd_address -offset 0x00000000 -range 0x20000000 -target_address_space [get_bd_addr_spaces axi_dma_0/Data_S2MM] [get_bd_addr_segs processing_system7_0/S_AXI_HP0/HP0_DDR_LOWOCM] -force

  current_bd_instance $oldCurInst
  validate_bd_design
  save_bd_design
}
# End of create_root_design

create_root_design ""
