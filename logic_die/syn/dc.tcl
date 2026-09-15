set root [file normalize [file join [file dirname [info script]] ../..]]
if {![info exists ::env(SS_TARGET_DB)] || ![file readable $::env(SS_TARGET_DB)]} {
    puts "ERROR: set SS_TARGET_DB to a readable local standard-cell library"; exit 1
}
set out [file normalize $::env(SS_DC_OUT)]
file mkdir $out
define_design_lib WORK -path $out/work
set hf $root/vortex-gpu/vortex/third_party/hardfloat/source
set_app_var search_path [concat [list $root/logic_die/rtl $hf $hf/RISCV] $search_path]
set_app_var target_library [list $::env(SS_TARGET_DB)]
set_app_var link_library [concat [list *] $target_library]
set_host_options -max_cores 4
set files [list $hf/HardFloat_primitives.v $hf/HardFloat_rawFN.v $hf/isSigNaNRecFN.v \
    $hf/fNToRecFN.v $hf/recFNToFN.v $hf/addRecFN.v $hf/mulRecFN.v $hf/RISCV/HardFloat_specialize.v \
    $root/logic_die/rtl/ld_hardfloat_clz.sv $root/logic_die/rtl/ld_fp32.sv \
    $root/logic_die/rtl/ld_weight_bank.sv $root/logic_die/rtl/ld_radix_topk.sv \
    $root/logic_die/rtl/logic_die_top.sv]
if {![analyze -format sverilog -define SYNTHESIS $files]} {exit 1}
if {![elaborate logic_die_top]} {exit 1}
current_design logic_die_top
if {![link]} {exit 1}
uniquify
set period 4.0
if {[info exists ::env(SS_CLOCK_NS)]} {set period $::env(SS_CLOCK_NS)}
create_clock -name logic_clk -period $period [get_ports clk]
set_clock_uncertainty 0.10 [get_clocks logic_clk]
set_input_delay 0.20 -clock logic_clk [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay 0.20 -clock logic_clk [all_outputs]
set_input_transition 0.05 [remove_from_collection [all_inputs] [get_ports clk]]
set_load 0.01 [all_outputs]
set_max_area 0
redirect $out/check_pre.rpt {check_design}
compile_ultra -no_autoungroup
write -format ddc -hierarchy -output $out/logic_die.ddc
write -format verilog -hierarchy -output $out/logic_die_mapped.v
write_sdc $out/logic_die.sdc
source $root/logic_die/syn/report.tcl
exit 0
