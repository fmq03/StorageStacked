# Called after compile, or on a saved DDC by reports_only.tcl.
redirect $out/check_post.rpt {check_design}
redirect $out/timing.rpt {report_timing -max_paths 20 -input_pins -nets -transition_time -capacitance}
redirect $out/hold.rpt {report_timing -delay_type min -max_paths 20}
redirect $out/area.rpt {report_area -hierarchy}
redirect $out/qor.rpt {report_qor}
redirect $out/constraints.rpt {report_constraint -all_violators}
redirect $out/power_estimate.rpt {report_power -analysis_effort low}
set paths [get_timing_paths -max_paths 1]
set hold_paths [get_timing_paths -delay_type min -max_paths 1]
if {[sizeof_collection $paths]!=1 || [sizeof_collection $hold_paths]!=1} {
    puts "ERROR: timing path missing"; exit 1
}
set slack [get_attribute $paths slack]
set hold_slack [get_attribute $hold_paths slack]
set leaf [get_cells -hierarchical -filter {is_hierarchical==false}]
set area 0.0
foreach a [get_attribute $leaf area] {set area [expr {$area+$a}]}
set cells [sizeof_collection $leaf]
set unmapped [sizeof_collection [get_cells -hierarchical -filter {is_unmapped==true}]]
set fp [open $out/summary.json w]
puts $fp [format {{"clock_ns":%.6f,"area_um2":%.6f,"worst_slack_ns":%.6f,"worst_hold_slack_ns":%.6f,"leaf_cells":%d,"unmapped_cells":%d,"memory_implementation":"standard_cell_registers","power_status":"vectorless_estimate_only"}} $period $area $slack $hold_slack $cells $unmapped]
close $fp
puts "SS_DC_COMPLETE"
