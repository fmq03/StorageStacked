# ==============================================================================
# Script: check_pin_assignments.tcl
# Description: Checks that all top-level HDL ports have a physical pin assigned.
#              Logs any missing assignments and forcefully exits Vivado if > 0.
# ==============================================================================

puts "\n========================================================="
puts " Running Early Pin Assignment Check..."
puts "========================================================="

# 1. Ensure a design is actually loaded into memory before checking
if {[current_design -quiet] eq ""} {
    puts "ERROR: No active design found. Please open a synthesized or elaborated design first."
    exit 1
}

# 2. Check for missing pin assignments manually
puts "DEBUG: Fetching all top-level ports and checking PACKAGE_PIN properties..."
set all_ports [get_ports *]

# Base ignore list (currently empty, but extensible)
set ignore_patterns {}

# Conditionally ignore DDR data/strobe pins and DDR sys_clk ONLY if using the DDR stub
if {[llength [get_files -quiet {*pins_ddr_stub.xdc}]] > 0} {
    puts "INFO: DDR stub detected. Ignoring unconstrained DDR data and DDR sys_clk ports."
    lappend ignore_patterns "*ddr4_dq*"
    lappend ignore_patterns "*ddr4_dqs*"
    lappend ignore_patterns "*c0_sys_clk*"
}

set missing_pins {}

foreach port $all_ports {
    # Check if the port matches any ignore pattern
    set ignored 0
    foreach pattern $ignore_patterns {
        if {[string match $pattern [get_property NAME $port]]} {
            set ignored 1
            break
        }
    }

    if {$ignored} { continue }

    set pin [get_property PACKAGE_PIN $port]
    # Ignore Vivado internal empty properties
    if {$pin eq "" || $pin eq "unassigned"} {
        lappend missing_pins $port
    }
}

# 3. Output and Exit Logic
set num_missing [llength $missing_pins]
if {$num_missing > 0} {
    puts "\n========================================================="
    puts " ERROR: $num_missing top-level ports are missing physical pin assignments!"
    puts " These ports MUST be constrained in an XDC file."
    puts "---------------------------------------------------------"

    foreach port $missing_pins {
        puts "   - $port"
    }

    puts "=========================================================\n"
    # send_msg_id can be finicky without a registered message database, so error out manually
    return -code error "Build failed due to missing pin assignments."
}

puts "\n========================================================="
puts " SUCCESS: Post-Synthesis Pin Check Passed. All required ports are assigned."
puts "========================================================="
