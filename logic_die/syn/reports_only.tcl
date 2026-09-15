set root [file normalize [file join [file dirname [info script]] ../..]]
set out [file normalize $::env(SS_DC_OUT)]
set period $::env(SS_CLOCK_NS)
set_app_var target_library [list $::env(SS_TARGET_DB)]
set_app_var link_library [concat [list *] $target_library]
read_ddc $out/logic_die.ddc
current_design logic_die_top
if {![link]} {exit 1}
source $root/logic_die/syn/report.tcl
exit 0
