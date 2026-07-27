#!/usr/bin/env wish
# AI Digital v0.6.8 — Native Tcl/Tk GUI (pipe-integrated CLI mode)
# Debug: all output goes to stderr AND gui_debug.log

package require Tk
catch {package require Ttk}
catch {tk useinputmethods 1}

# ===================== Debug System =====================
set ::debug_chan stderr
proc _debug_init {} {
    set script_dir [file dirname [info script]]
    set logpath [file normalize [file join $script_dir "gui_debug.log"]]
    if {[catch {set f [open $logpath w]}]} { return }
    set ::debug_chan $f
    _debug_write "DEBUG" "Debug log initialized: $logpath"
}
proc _debug_write {level msg} {
    set ts [clock format [clock seconds] -format "%Y-%m-%d %H:%M:%S"]
    set line "\[$level\] $ts $msg"
    puts stderr $line
    if {[info exists ::debug_chan] && $::debug_chan ne "stderr"} {
        catch {puts $::debug_chan $line; flush $::debug_chan}
    }
}
proc debug {msg} { _debug_write "DEBUG" $msg }
proc debug_error {ctx msg} { _debug_write "ERROR" "$ctx: $msg" }
proc bgerror {msg} { _debug_write "ERROR" "Background: $msg -> $::errorInfo" }

proc focus_console_input {} {
    if {![winfo exists .console.input]} { return }
    catch {tk useinputmethods 1}
    focus .console.input
    if {[lsearch -exact {Entry TEntry} [winfo class .console.input]] >= 0} {
        catch {.console.input icursor end}
        catch {.console.input xview end}
    } else {
        catch {.console.input mark set insert end-1c}
        catch {.console.input see insert}
    }
}

proc console_input_get {} {
    if {![winfo exists .console.input]} { return "" }
    if {[lsearch -exact {Entry TEntry} [winfo class .console.input]] >= 0} {
        return [string trim [.console.input get]]
    }
    return [string trimright [.console.input get 1.0 "end-1c"] "\n"]
}

proc console_input_clear {} {
    if {![winfo exists .console.input]} { return }
    if {[lsearch -exact {Entry TEntry} [winfo class .console.input]] >= 0} {
        .console.input delete 0 end
    } else {
        .console.input delete 1.0 end
    }
}

_debug_init
debug "=== AI Digital GUI v0.6.8 (pipe mode) ==="
debug "Tcl=[info tclversion] Tk=[package provide Tk]"

# ===================== Config =====================
set ::APP_NAME "AI Digital"
set ::APP_VERSION "0.6.8"
set ::BINARY [file normalize [file join [file dirname [info script]] "target" "release" "ai_digital"]]
if {![file exists $::BINARY]} {
    set ::BINARY [file normalize [file join [file dirname [info script]] "target" "debug" "ai_digital"]]
}
if {![file exists $::BINARY]} {
    set ::BINARY [file normalize [file join [file dirname [info script]] ".." "target" "release" "ai_digital"]]
}
if {![file exists $::BINARY]} {
    set ::BINARY [file normalize [file join [file dirname [info script]] ".." "target" "debug" "ai_digital"]]
}
if {![file exists $::BINARY]} { set ::BINARY "ai_digital" }
debug "Binary: $::BINARY"
debug "Binary exists: [file exists $::BINARY]"

# ===================== Color Theme =====================
set ::C_BG        "#1e1e2e"
set ::C_PANEL     "#252536"
set ::C_ACCENT    "#2a2a3e"
set ::C_HIGHLIGHT "#4fc3f7"
set ::C_TEXT      "#e0e0e0"
set ::C_DIM       "#888899"
set ::C_OK        "#66bb6a"
set ::C_ERR       "#ef5350"
set ::C_WARN      "#ffa726"
set ::C_INPUT_BG  "#1a1a28"
set ::C_BORDER    "#3a3a4e"
set ::C_TITLE_BG  "#1a1a2e"

catch {
    ttk::style theme use clam
    ttk::style configure ConsoleInput.TEntry \
        -fieldbackground $::C_INPUT_BG \
        -foreground $::C_TEXT \
        -insertcolor $::C_HIGHLIGHT \
        -bordercolor $::C_BORDER \
        -lightcolor $::C_BORDER \
        -darkcolor $::C_BORDER \
        -padding {8 5}
    ttk::style map ConsoleInput.TEntry \
        -fieldbackground [list focus $::C_INPUT_BG !focus $::C_INPUT_BG] \
        -foreground [list focus $::C_TEXT !focus $::C_TEXT]
}

# ===================== State =====================
set ::current_module ""
set ::current_rtl ""
set ::project_name "default"
set ::current_page "rtl"
set ::console_lines 0
set ::constraint_freq 100
set ::synth_cells 0; set ::synth_dff 0; set ::synth_wires 0
set ::synth_area_ge 0.0; set ::synth_area_um2 0.0; set ::synth_depth 0
set ::synth_gate_netlist ""
set ::timing_slack 0.0; set ::timing_met 0
set ::power_total 0.0; set ::power_static 0.0; set ::power_dynamic 0.0
set ::power_corner_rows {}
set ::area_breakdown_rows {}
set ::formal_result ""
set ::formal_points ""
set ::gate_dot ""
set ::gate_zoom 1.0
set ::gate_parsed_netlist ""
set ::timing_paths {}
set ::timing_selected_path 1
set ::timing_zoom 1.0
set ::wave_zoom 1.0
set ::wave_data ""
set ::wave_signal_map ""
set ::wave_visible_signals {}
set ::wave_signal_order {}
set ::wave_max_time 0
set ::hierarchy_rows {}
set ::formal_points_data ""
set ::pipe_id ""
set ::pipe_buffer ""
set ::pipe_ready 0
set ::pipe_last_cmd ""
set ::current_project_dir ""
set ::project_state_path ""
set ::active_technology ""
set ::technology_rows {}
set ::technology_names {}
set ::selected_technology ""
set ::power_corner_rows {}

proc hex_decode {value} {
    if {$value eq ""} { return "" }
    if {[catch {binary decode hex $value} decoded]} {
        return $value
    }
    return $decoded
}

proc gui_state_get {key {default ""}} {
    if {[info exists ::gui_state($key)]} {
        return $::gui_state($key)
    }
    return $default
}

proc gui_state_text {key} {
    return [gui_state_get $key ""]
}

proc load_text_file {path} {
    if {$path eq "" || ![file exists $path]} { return "" }
    if {[catch {set fp [open $path r]; set content [read $fp]; close $fp} err]} {
        debug_error "load_text_file" "$path: $err"
        return ""
    }
    return $content
}

proc load_exchange_file {path widget} {
    if {![winfo exists $widget]} { return }
    set content [load_text_file $path]
    $widget configure -state normal
    $widget delete 1.0 end
    if {$content ne ""} {
        $widget insert end $content
    }
    $widget configure -state disabled
}

proc load_gui_state_file {path} {
    if {$path eq "" || ![file exists $path]} { return 0 }
    debug "load_gui_state_file: $path"
    catch {unset ::gui_state}
    if {[catch {source $path} err]} {
        debug_error "load_gui_state_file" $err
        return 0
    }
    set ::project_name [gui_state_text project_name]
    if {$::project_name eq ""} { set ::project_name "default" }
    set ::current_module [gui_state_text module_name]
    set ::current_project_dir [gui_state_text project_dir]
    set ::project_state_path $path
    if {[winfo exists .toparea.left.project]} {
        .toparea.left.project configure -text "  $::project_name"
    }
    if {[winfo exists .toparea.left.module]} {
        .toparea.left.module configure -text "  [expr {$::current_module eq "" ? "(none)" : $::current_module}]"
    }
    if {[info exists ::gui_state(constraint_freq_mhz)]} {
        set ::constraint_freq $::gui_state(constraint_freq_mhz)
    }
    if {[info exists ::gui_state(cell_count)]} { set ::synth_cells $::gui_state(cell_count) }
    if {[info exists ::gui_state(dff_count)]} { set ::synth_dff $::gui_state(dff_count) }
    if {[info exists ::gui_state(wire_count)]} { set ::synth_wires $::gui_state(wire_count) }
    if {[info exists ::gui_state(area_ge)]} { set ::synth_area_ge $::gui_state(area_ge) }
    if {[info exists ::gui_state(area_um2)]} { set ::synth_area_um2 $::gui_state(area_um2) }
    if {[info exists ::gui_state(logic_depth)]} { set ::synth_depth $::gui_state(logic_depth) }
    if {[info exists ::gui_state(timing_slack_ns)]} { set ::timing_slack $::gui_state(timing_slack_ns) }
    if {[info exists ::gui_state(timing_met)]} { set ::timing_met $::gui_state(timing_met) }
    if {[info exists ::gui_state(power_total_mw)]} { set ::power_total $::gui_state(power_total_mw) }
    if {[info exists ::gui_state(power_static_mw)]} { set ::power_static $::gui_state(power_static_mw) }
    if {[info exists ::gui_state(power_dynamic_mw)]} { set ::power_dynamic $::gui_state(power_dynamic_mw) }
    if {[info exists ::gui_state(formal_status)]} { set ::formal_result [gui_state_text formal_status] }
    set ::active_technology [gui_state_text active_technology]
    if {[winfo exists .status.text]} {
        .status.text configure -text "  AI Digital v$::APP_VERSION | [gui_state_text current_step] / [gui_state_text step_status] | [gui_state_text status_text]"
    }
    update_synth_stats
    update_timing_page
    update_formal_page
    update_sim_page
    update_power_page
    update_area_page
    update_summary_page
    load_exchange_file [gui_state_text exchange_rtl_path] .toparea.right.pages.rtl.code
    load_exchange_file [gui_state_text tb_path] .toparea.right.pages.rtl.tb
    load_exchange_file [gui_state_text exchange_gate_path] .toparea.right.pages.synth.gate_text
    load_exchange_file [gui_state_text exchange_sim_path] .toparea.right.pages.sim.out
    load_exchange_file [gui_state_text exchange_timing_path] .toparea.right.pages.timing.out
    load_exchange_file [gui_state_text exchange_formal_path] .toparea.right.pages.formal.out
    load_exchange_file [gui_state_text exchange_area_path] .toparea.right.pages.area.out
    load_exchange_file [gui_state_text exchange_power_path] .toparea.right.pages.power.main.report.out
    load_exchange_file [gui_state_text exchange_summary_path] .toparea.right.pages.summary.out
    if {[gui_state_text gate_path] ne ""} {
        load_gate_from_project
    }
    load_timing_paths_from_state
    load_waveform_from_state
    load_hierarchy_from_state
    load_formal_points_from_state
    load_technology_from_state
    load_power_corners_from_state
    return 1
}

proc handle_gui_event {line} {
    if {![regexp {^@@GUI_EVENT\tSTATE\tpath_hex=([0-9a-fA-F]+)(?:\tstep=([^\t]+))?(?:\tstatus=([^\t]+))?} $line -> path_hex step status]} {
        return 0
    }
    set state_path [hex_decode $path_hex]
    if {$state_path eq ""} { return 0 }
    debug "gui event state: $state_path"
    load_gui_state_file $state_path
    if {$step ne "" && [winfo exists .status.text]} {
        .status.text configure -text "  AI Digital v$::APP_VERSION | $step / $status"
    }
    return 1
}

# ===================== Pipe Integration =====================
proc pipe_open {} {
    # Get absolute paths BEFORE changing directory
    set script_dir [file normalize [file dirname [info script]]]
    set project_root $script_dir

    # Try config.yaml in project directory first (where gui.tcl is located)
    set config_path [file join $script_dir "config.yaml"]
    # Fallback to project root if not found
    if {![file exists $config_path]} {
        set config_path [file join $project_root "config.yaml"]
    }

    # Set working directory to project root so CLI binary finds workspace/
    if {[file isdirectory $project_root]} {
        cd $project_root
        debug "pipe_open: cwd=[pwd]"
    }

    # Start CLI pipe mode with structured GUI events
    set cmd "$::BINARY --pipe"
    if {[file exists $config_path]} {
        set cmd "$::BINARY --pipe --config $config_path"
        debug "pipe_open: using config file: $config_path"
    }
    debug "pipe_open: spawning $cmd (pipe mode)"
    if {[catch {set ::pipe_id [open "|$cmd 2>@1" r+]} err]} {
        debug_error "pipe_open" "Failed to spawn: $err"
        console_log "  Error: Cannot start CLI subprocess" "err"
        return 0
    }
    fconfigure $::pipe_id -buffering none -blocking 0
    fileevent $::pipe_id readable [list pipe_read]
    set ::pipe_buffer ""
    set ::pipe_ready 0
    debug "pipe_open: OK (fd=$::pipe_id)"
    return 1
}

proc pipe_send {cmd} {
    if {$::pipe_id eq ""} { return }
    set ::pipe_last_cmd $cmd
    set ::pipe_ready 0
    debug "pipe_send: $cmd"
    if {[catch {puts $::pipe_id $cmd; flush $::pipe_id} err]} {
        debug_error "pipe_send" $err
        console_log "  Error: pipe write failed" "err"
        return
    }
}

proc pipe_read {} {
    if {$::pipe_id eq ""} { return }
    if {[eof $::pipe_id]} {
        debug "pipe_read: EOF"
        set chan $::pipe_id
        catch {close $::pipe_id}
        set ::pipe_id ""
        catch {fileevent $chan readable {}}
        console_log "  CLI subprocess disconnected" "err"
        return
    }
    if {[catch {set chunk [read $::pipe_id]} err]} {
        debug_error "pipe_read" $err
        return
    }
    if {$chunk eq ""} { return }
    debug "pipe_read: [string length $chunk] bytes"
    append ::pipe_buffer $chunk

    # Process complete lines
    while {[set nl [string first "\n" $::pipe_buffer]] >= 0} {
        set line [string range $::pipe_buffer 0 $nl-1]
        set ::pipe_buffer [string range $::pipe_buffer $nl+1 end]
        pipe_process_line $line
    }

    # Check if remaining buffer is a prompt (no trailing newline)
    # Interactive mode prompt looks like: "ai_digital [project] ▸ " (with trailing space)
    if {[regexp {ai_digital\s+\[.*?\]\s+▸\s*$} $::pipe_buffer]} {
        set ::pipe_ready 1
        set ::pipe_buffer ""
        # Trigger any pending follow-up actions
        pipe_on_ready
    }
}

proc pipe_process_line {line} {
    if {$line eq ""} { return }

    # Strip ANSI escape sequences for parsing
    set plain [regsub -all {\x1b\[[0-9;]*[a-zA-Z]} $line ""]

    # Debug: log what we're parsing
    debug "parse: $plain"

    if {[handle_gui_event $plain]} {
        return
    }

    # Skip prompt echoes (the line "ai_digital [project] ▸ command")
    # But don't skip the initial prompt line
    if {[regexp {^ai_digital.*▸\s+\S} $plain]} {
        # This is the echo of our command - skip it
        return
    }

    # Parse module info from plain text
    if {[regexp {module:\s*(\w+)} $plain -> mod]} {
        set ::current_module $mod
        catch {.toparea.left.module configure -text "  $mod"}
        debug "Module detected: $mod"
    }
    # Parse project info
    if {[regexp {project:\s*(\w+)} $plain -> proj]} {
        set ::project_name $proj
        catch {.toparea.left.project configure -text "  $proj"}
        debug "Project detected: $proj"
    }

    # Parse synthesis results - multiple formats
    # Format 1: "Cells: 5"
    if {[regexp {^\s*Cells:\s*(\d+)} $plain -> cells]} {
        set ::synth_cells $cells
        update_synth_stats
        debug "Cells: $cells"
    }
    # Format 2: "Wires: 9  Ports: 4  Cells: 5"
    if {[regexp {Wires:\s*(\d+)\s+Ports:\s*(\d+)\s+Cells:\s*(\d+)} $plain -> wires ports cells]} {
        set ::synth_wires $wires
        set ::synth_cells $cells
        update_synth_stats
        debug "Wires: $wires, Ports: $ports, Cells: $cells"
    }
    # Wires alone
    if {[regexp {^\s*Wires:\s*(\d+)} $plain -> wires]} {
        set ::synth_wires $wires
        debug "Wires: $wires"
    }
    # Area formats
    if {[regexp {Area:\s*([\d.]+)\s*GE} $plain -> area]} {
        set ::synth_area_ge $area
        update_synth_stats
        debug "Area: $area GE"
    }
    # Logic Depth
    if {[regexp {Logic Depth:\s*(\d+)} $plain -> depth]} {
        set ::synth_depth $depth
        update_synth_stats
        debug "Depth: $depth"
    }

    # Synthesis completed → load gate netlist from the project output
    if {[regexp {✓ Synthesis completed} $plain]} {
        after 500 [list load_gate_from_project]
        debug "Synthesis completed, will load gate netlist"
    }

    # Parse DFF count from cell breakdown lines like "$_DFFSR_PPP_: 4"
    if {[regexp {^\s*\$_(\w*DFF\w*)\s*:\s*(\d+)} $plain -> dff_type dff_count]} {
        set ::synth_dff [expr {$::synth_dff + $dff_count}]
        update_synth_stats
        debug "DFF detected: $dff_type = $dff_count (total: $::synth_dff)"
    }

    # Parse Stats line: "Stats: 5 total (4 DFFs / 1 combinational)"
    if {[regexp {Stats:\s*(\d+)\s+total\s+\((\d+)\s+DFFs\s*/\s*(\d+)\s+combinational\)} $plain -> total dffs comb]} {
        set ::synth_cells $total
        set ::synth_dff $dffs
        update_synth_stats
        debug "Stats: $total total, $dffs DFFs, $comb comb"
    }

    # Parse timing results - multiple formats
    # Format 1: "Slack: 9.60 ns"
    if {[regexp {Slack:\s*([-\d.]+)\s*ns} $plain -> slack]} {
        set ::timing_slack $slack
        set ::timing_met [expr {$slack >= 0}]
        update_timing_page
        debug "Timing Slack: $slack ns (met: $::timing_met)"
    }
    # Format 2: "Arrival: 0.25 ns  Required: 9.85 ns  Slack: 9.60 ns"
    if {[regexp {Arrival:\s*([\d.]+)\s*ns\s+Required:\s*([\d.]+)\s*ns\s+Slack:\s*([-\d.]+)\s*ns} $plain -> arrival req slack]} {
        set ::timing_slack $slack
        set ::timing_met [expr {$slack >= 0}]
        update_timing_page
        debug "Timing: arrival=$arrival, required=$req, slack=$slack"
    }

    # Parse power results
    if {[regexp {Power:\s*([\d.]+)\s*mW} $plain -> pwr]} {
        set ::power_total $pwr
        debug "Power: $pwr mW"
    }

    # Parse formal result - multiple formats
    # Format 1: "PASS" or "FAIL" at line start
    if {[regexp {^\s*(PASS|FAIL|EQUIVALENT|DIFFERENT)} $plain -> status]} {
        set ::formal_result $status
        update_formal_page
        debug "Formal result: $status"
    }
    # Format 2: "Formal Verification: PASS"
    if {[regexp {Formal Verification:\s*(PASS|FAIL)} $plain -> status]} {
        set ::formal_result $status
        update_formal_page
        debug "Formal verification: $status"
    }

    # Parse simulation results
    # "✓ Simulation PASSED"
    if {[regexp {✓ Simulation (PASSED|FAILED)} $plain -> status]} {
        debug "Simulation: $status"
    }

    # Display in console with ANSI color conversion
    console_log_ansi "$line"
}

proc console_log_ansi {line} {
    if {![winfo exists .console.text]} { return }
    .console.text configure -state normal

    # Parse ANSI escape sequences and convert to Tk tags
    set pos 0
    set len [string length $line]
    set current_tag ""

    while {$pos < $len} {
        # Find next ANSI escape sequence
        set esc_pos [string first "\x1b\[" $line $pos]

        if {$esc_pos == -1} {
            # No more escape sequences - insert remaining text
            set text [string range $line $pos end]
            if {$current_tag ne ""} {
                .console.text insert end $text $current_tag
            } else {
                .console.text insert end $text
            }
            break
        }

        # Insert text before escape sequence
        if {$esc_pos > $pos} {
            set text [string range $line $pos [expr {$esc_pos - 1}]]
            if {$current_tag ne ""} {
                .console.text insert end $text $current_tag
            } else {
                .console.text insert end $text
            }
        }

        # Find end of escape sequence
        set m_pos [string first "m" $line $esc_pos]
        if {$m_pos == -1} { break }

        # Extract ANSI code
        set code [string range $line [expr {$esc_pos + 2}] [expr {$m_pos - 1}]]

        # Map ANSI codes to Tk tags
        switch -glob -- $code {
            "0" { set current_tag "" }
            "1" { set current_tag "bold_tag" }
            "2" { set current_tag "dim_tag" }
            "3*" { set current_tag "green_tag" }
            "32" { set current_tag "green_tag" }
            "33" { set current_tag "yellow_tag" }
            "34" { set current_tag "blue_tag" }
            "35" { set current_tag "magenta_tag" }
            "36" { set current_tag "cyan_tag" }
            "91" { set current_tag "red_tag" }
            "92" { set current_tag "green_tag" }
            "93" { set current_tag "yellow_tag" }
            "94" { set current_tag "blue_tag" }
            "95" { set current_tag "magenta_tag" }
            "96" { set current_tag "cyan_tag" }
            default { set current_tag "" }
        }

        set pos [expr {$m_pos + 1}]
    }

    .console.text insert end "\n"
    .console.text see end
    .console.text configure -state disabled
    incr ::console_lines
}

proc pipe_on_ready {} {
    # Binary is ready for next command
    # Update status bar
    if {[winfo exists .status.module]} {
        if {$::current_module ne ""} {
            .status.module configure -text "  $::current_module  "
        }
    }
    # Enable input
    if {[winfo exists .console.input]} {
        .console.input configure -state normal
        focus_console_input
    }
}

# ===================== Native Gate Diagram / Waveform / Hierarchy =====================
proc canvas_message {canvas title subtitle} {
    if {![winfo exists $canvas]} { return }
    $canvas delete all
    set width [winfo width $canvas]
    if {$width < 200} { set width 700 }
    set height [winfo height $canvas]
    if {$height < 120} { set height 320 }
    $canvas create text [expr {$width / 2.0}] [expr {$height / 2.0 - 14}] \
        -text $title -fill $::C_HIGHLIGHT -font {Helvetica 14 bold}
    $canvas create text [expr {$width / 2.0}] [expr {$height / 2.0 + 14}] \
        -text $subtitle -fill $::C_DIM -font {Helvetica 10}
    $canvas configure -scrollregion "0 0 $width $height"
}

proc canvas_pan_mark {canvas x y} {
    if {![winfo exists $canvas]} { return }
    $canvas scan mark $x $y
}

proc canvas_pan_drag {canvas x y} {
    if {![winfo exists $canvas]} { return }
    $canvas scan dragto $x $y 1
}

proc bool_value {value} {
    expr {$value in {1 true TRUE yes YES}}
}

proc parse_decl_signals {decl} {
    set cleaned [string map {"," " " ";" " "} $decl]
    set result {}
    foreach token [split $cleaned " "] {
        set token [string trim $token]
        if {$token eq ""} { continue }
        if {[regexp {^\[[^]]+\]$} $token]} { continue }
        if {$token in {input output inout wire reg logic signed unsigned}} { continue }
        if {[regexp {^[A-Za-z_][A-Za-z0-9_]*(\[[^]]+\])?$} $token]} {
            lappend result $token
        }
    }
    return $result
}

proc strip_verilog_comments {text} {
    set out ""
    set in_block 0
    set length [string length $text]
    for {set i 0} {$i < $length} {incr i} {
        set ch [string index $text $i]
        set next [expr {$i + 1 < $length ? [string index $text [expr {$i + 1}]] : ""}]
        if {$in_block} {
            if {$ch eq "*" && $next eq "/"} {
                set in_block 0
                incr i
            }
            continue
        }
        if {$ch eq "/" && $next eq "*"} {
            set in_block 1
            incr i
            continue
        }
        if {$ch eq "/" && $next eq "/"} {
            while {$i < $length && [string index $text $i] ne "\n"} {
                incr i
            }
            append out "\n"
            continue
        }
        append out $ch
    }
    return $out
}

proc split_verilog_statements {text} {
    set cleaned [strip_verilog_comments $text]
    set stmts {}
    set current ""
    set paren_depth 0
    set bracket_depth 0
    set brace_depth 0
    foreach ch [split $cleaned ""] {
        append current $ch
        switch -- $ch {
            "(" { incr paren_depth }
            ")" { if {$paren_depth > 0} { incr paren_depth -1 } }
            "[" { incr bracket_depth }
            "]" { if {$bracket_depth > 0} { incr bracket_depth -1 } }
            "{" { incr brace_depth }
            "}" { if {$brace_depth > 0} { incr brace_depth -1 } }
            ";" {
                if {$paren_depth == 0 && $bracket_depth == 0 && $brace_depth == 0} {
                    set stmt [string trim $current]
                    if {$stmt ne ""} {
                        lappend stmts $stmt
                    }
                    set current ""
                }
            }
        }
    }
    set tail [string trim $current]
    if {$tail ne ""} {
        lappend stmts $tail
    }
    return $stmts
}

proc gate_pin_role {pin_name} {
    set upper [string toupper $pin_name]
    if {$upper in {Q QN Y Z ZN O CO S}} { return out }
    if {$upper in {C CK CLK CP G}} { return clock }
    if {$upper in {R RN RESET RESETN CLR CLRN SET SETN PREN SN}} { return reset }
    return in
}

proc gate_is_sequential {cell_type} {
    set upper [string toupper $cell_type]
    if {[string first "DFF" $upper] >= 0} { return 1 }
    if {[string first "LATCH" $upper] >= 0} { return 1 }
    if {[string first "SDFF" $upper] >= 0} { return 1 }
    return 0
}

proc gate_is_constant {signal_name} {
    expr {$signal_name in {0 1 1'b0 1'b1 1'h0 1'h1}}
}

proc gate_cell_family {cell_type} {
    set upper [string toupper $cell_type]
    if {[gate_is_sequential $cell_type]} { return dff }
    if {[string first "MUX" $upper] >= 0} { return mux }
    if {[string first "XNOR" $upper] >= 0 || [string first "XOR" $upper] >= 0} { return xor }
    if {[string first "NOR" $upper] >= 0} { return nor }
    if {[string first "NAND" $upper] >= 0} { return nand }
    if {[string first " OR" " $upper"] >= 0 || [string first "_OR_" $upper] >= 0 || [regexp {(^|[^A-Z])OR([^A-Z]|$)} $upper]} { return or }
    if {[string first "AND" $upper] >= 0} { return and }
    if {[string first "INV" $upper] >= 0 || [string first "NOT" $upper] >= 0} { return inv }
    if {[string first "BUF" $upper] >= 0} { return buf }
    return box
}

proc gate_draw_text_with_bg {canvas x y text anchor fill font} {
    set text_id [$canvas create text $x $y -text $text -anchor $anchor -fill $fill -font $font]
    set bbox [$canvas bbox $text_id]
    if {[llength $bbox] == 4} {
        lassign $bbox x1 y1 x2 y2
        set bg_id [$canvas create rectangle [expr {$x1 - 2}] [expr {$y1 - 1}] [expr {$x2 + 2}] [expr {$y2 + 1}] \
            -fill $::C_INPUT_BG -outline ""]
        $canvas lower $bg_id $text_id
    }
    return $text_id
}

proc gate_draw_line_label {canvas x y text anchor fill} {
    gate_draw_text_with_bg $canvas $x $y $text $anchor $fill {Helvetica 7}
}

proc gate_clamp {value min_value max_value} {
    if {$value < $min_value} { return $min_value }
    if {$value > $max_value} { return $max_value }
    return $value
}

proc gate_spread_row {index total slots} {
    if {$total <= 1 || $slots <= 1} { return 0 }
    expr {round($index * (($slots - 1.0) / double($total - 1)))}
}

proc gate_centered_item_y {top region_h count index pitch item_h} {
    if {$count < 1} { set count 1 }
    if {$index < 0} { set index 0 }
    set used_h $item_h
    if {$count > 1} {
        set used_h [expr {$item_h + ($count - 1) * $pitch}]
    }
    set offset [expr {($region_h - $used_h) / 2.0}]
    if {$offset < 0} { set offset 0.0 }
    expr {$top + $offset + $index * $pitch}
}

proc gate_compute_lane_x {sx tx unit lane_idx} {
    set stub [expr {18.0 * $unit}]
    set gutter [expr {12.0 * $unit}]
    set left [expr {min($sx, $tx) + $stub + $gutter}]
    set right [expr {max($sx, $tx) - $stub - $gutter}]
    if {$right <= $left} {
        return [expr {($sx + $tx) / 2.0}]
    }
    set span [expr {$right - $left}]
    set slots [expr {int($span / (22.0 * $unit))}]
    if {$slots < 1} { set slots 1 }
    set slot [expr {$lane_idx % ($slots + 1)}]
    expr {$left + (($slot + 1) * $span / double($slots + 2))}
}

proc gate_compute_lane_y {sy ty unit lane_idx} {
    set base [expr {($sy + $ty) / 2.0}]
    set band [expr {14.0 * $unit}]
    set slot [expr {$lane_idx % 5}]
    expr {$base + ($slot - 2) * $band}
}

proc gate_build_fanout_map {netlist outputs} {
    set fanout [dict create]
    foreach cell [dict get $netlist cells] {
        foreach pin [dict get $cell inputs] {
            dict incr fanout [dict get $pin signal] 1
        }
    }
    foreach sig $outputs {
        dict incr fanout $sig 1
    }
    return $fanout
}

proc gate_should_label_net {signal_name fanout_map inputs outputs} {
    if {[gate_is_constant $signal_name]} { return 0 }
    if {[lsearch -exact $outputs $signal_name] >= 0} { return 1 }
    if {[regexp -nocase {(^|[._/])clk($|[._/\[])|clock|rst|reset} $signal_name]} { return 1 }
    if {[lsearch -exact $inputs $signal_name] >= 0} { return 0 }
    if {[dict exists $fanout_map $signal_name]
        && [dict get $fanout_map $signal_name] > 4
        && ![string match "_*" $signal_name]
        && [string length $signal_name] <= 18} {
        return 1
    }
    return 0
}

proc gate_draw_pin_marker {canvas x y color unit} {
    set r [expr {2.2 * $unit}]
    $canvas create oval [expr {$x - $r}] [expr {$y - $r}] [expr {$x + $r}] [expr {$y + $r}] \
        -fill $color -outline ""
}

proc gate_estimate_cell_height {cell unit} {
    set pin_count [llength [dict get $cell pins]]
    if {$pin_count < 2} { set pin_count 2 }
    expr {56.0 * $unit + $pin_count * 10.0 * $unit}
}

proc gate_draw_connection {canvas sx sy tx ty color width label label_fill unit route_x1 route_x2 route_y} {
    $canvas create line $sx $sy $route_x1 $sy -fill $color -width $width -capstyle round -joinstyle round
    if {abs($route_y - $sy) > 0.5} {
        $canvas create line $route_x1 $sy $route_x1 $route_y -fill $color -width $width -capstyle round -joinstyle round
    }
    $canvas create line $route_x1 $route_y $route_x2 $route_y -fill $color -width $width -capstyle round -joinstyle round
    if {abs($route_y - $ty) > 0.5} {
        $canvas create line $route_x2 $route_y $route_x2 $ty -fill $color -width $width -capstyle round -joinstyle round
    }
    $canvas create line $route_x2 $ty $tx $ty -fill $color -width $width -capstyle round -joinstyle round
    gate_draw_pin_marker $canvas $sx $sy $color $unit
    gate_draw_pin_marker $canvas $tx $ty $color $unit

    if {$label ne ""} {
        set wire_x [expr {($route_x1 + $route_x2) / 2.0}]
        set wire_y $route_y
        set label_x $wire_x
        set label_y [expr {$route_y - 12 * $unit}]
        set anchor center
        $canvas create line $wire_x $wire_y $label_x $label_y -fill "#506070" -width 0.8
        gate_draw_line_label $canvas $label_x $label_y $label $anchor $label_fill
    }
}

proc gate_draw_cell {canvas box cell_type inst unit} {
    set x [dict get $box x]
    set y [dict get $box y]
    set w [dict get $box w]
    set h [dict get $box h]
    set family [gate_cell_family $cell_type]
    set sx1 [expr {$x + 26 * $unit}]
    set sx2 [expr {$x + $w - 26 * $unit}]
    set sy1 [expr {$y + 18 * $unit}]
    set sy2 [expr {$y + $h - 18 * $unit}]
    set cy [expr {($sy1 + $sy2) / 2.0}]
    set stroke "#7fd7ff"
    if {$family ni {dff box}} {
        set stroke "#64d6d6"
    }
    $canvas create rectangle $x $y [expr {$x + $w}] [expr {$y + $h}] \
        -fill "#20293a" -outline "#78c8ff" -width 1.4
    $canvas create rectangle [expr {$x + 4 * $unit}] [expr {$y + 4 * $unit}] \
        [expr {$x + $w - 4 * $unit}] [expr {$y + $h - 4 * $unit}] \
        -outline "#31455c" -width 1.0
    gate_draw_text_with_bg $canvas [expr {$x + 8 * $unit}] [expr {$y + 8 * $unit}] $cell_type nw "#d8f3ff" {Helvetica 8 bold}
    gate_draw_text_with_bg $canvas [expr {$x + 8 * $unit}] [expr {$y + $h - 8 * $unit}] $inst sw $::C_TEXT {Helvetica 8}

    switch -- $family {
        dff {
            $canvas create rectangle [expr {$sx1 + 6 * $unit}] $sy1 [expr {$sx2 - 4 * $unit}] $sy2 \
                -fill "#1f3648" -outline "#6cc4ff" -width 1.4
            $canvas create polygon \
                [expr {$sx1 + 6 * $unit}] $cy \
                [expr {$sx1 + 18 * $unit}] [expr {$cy - 7 * $unit}] \
                [expr {$sx1 + 18 * $unit}] [expr {$cy + 7 * $unit}] \
                -fill $::C_INPUT_BG -outline "#6cc4ff" -width 1.2
            gate_draw_text_with_bg $canvas [expr {$sx1 + 24 * $unit}] $cy "D" w "#d8f3ff" {Helvetica 9 bold}
            gate_draw_text_with_bg $canvas [expr {$sx2 - 16 * $unit}] $cy "Q" e "#d8f3ff" {Helvetica 9 bold}
            gate_draw_text_with_bg $canvas [expr {$sx1 + 16 * $unit}] [expr {$sy2 - 10 * $unit}] "CLK" w "#8bd0ff" {Helvetica 7 bold}
        }
        mux {
            $canvas create rectangle [expr {$sx1 - 6 * $unit}] $sy1 [expr {$sx2 + 2 * $unit}] $sy2 \
                -fill "#233041" -outline "#324156" -width 1.0
            $canvas create polygon \
                [expr {$sx1 + 4 * $unit}] $sy1 \
                [expr {$sx2 - 10 * $unit}] [expr {$sy1 + 8 * $unit}] \
                [expr {$sx2 - 10 * $unit}] [expr {$sy2 - 8 * $unit}] \
                [expr {$sx1 + 4 * $unit}] $sy2 \
                -fill "#233041" -outline $stroke -width 1.5
        }
        inv -
        buf {
            $canvas create polygon \
                $sx1 $sy1 \
                $sx1 $sy2 \
                [expr {$sx2 - 14 * $unit}] $cy \
                -fill "#233041" -outline $stroke -width 1.5
            if {$family eq "inv"} {
                $canvas create oval [expr {$sx2 - 14 * $unit}] [expr {$cy - 5 * $unit}] \
                    [expr {$sx2 - 4 * $unit}] [expr {$cy + 5 * $unit}] \
                    -fill $::C_INPUT_BG -outline $stroke -width 1.4
            }
        }
        and -
        nand {
            set arc_left [expr {$sx2 - ($sy2 - $sy1)}]
            $canvas create line $sx1 $sy1 $arc_left $sy1 -fill $stroke -width 1.5
            $canvas create line $sx1 $sy2 $arc_left $sy2 -fill $stroke -width 1.5
            $canvas create line $sx1 $sy1 $sx1 $sy2 -fill $stroke -width 1.5
            $canvas create arc [expr {$arc_left - 2 * $unit}] $sy1 [expr {$sx2 + 2 * $unit}] $sy2 \
                -start 270 -extent 180 -style arc -outline $stroke -width 1.5
            if {$family eq "nand"} {
                $canvas create oval [expr {$sx2 - 3 * $unit}] [expr {$cy - 5 * $unit}] \
                    [expr {$sx2 + 7 * $unit}] [expr {$cy + 5 * $unit}] \
                    -fill $::C_INPUT_BG -outline $stroke -width 1.4
            }
        }
        or -
        nor -
        xor {
            if {$family eq "xor"} {
                $canvas create line [expr {$sx1 - 6 * $unit}] $sy1 [expr {$sx1 + 6 * $unit}] $cy [expr {$sx1 - 6 * $unit}] $sy2 \
                    -smooth 1 -fill $stroke -width 1.2
            }
            $canvas create line $sx1 $sy1 [expr {$sx1 + 18 * $unit}] $cy $sx1 $sy2 \
                -smooth 1 -fill $stroke -width 1.4
            $canvas create line [expr {$sx1 + 10 * $unit}] $sy1 $sx2 $cy [expr {$sx1 + 10 * $unit}] $sy2 \
                -smooth 1 -fill $stroke -width 1.5
            if {$family eq "nor"} {
                $canvas create oval [expr {$sx2 - 2 * $unit}] [expr {$cy - 5 * $unit}] \
                    [expr {$sx2 + 8 * $unit}] [expr {$cy + 5 * $unit}] \
                    -fill $::C_INPUT_BG -outline $stroke -width 1.4
            }
        }
        default {
            $canvas create rectangle [expr {$sx1 - 6 * $unit}] $sy1 [expr {$sx2 + 2 * $unit}] $sy2 \
                -fill "#29293f" -outline "#60d4d4" -width 1.4
            gate_draw_text_with_bg $canvas [expr {$x + $w / 2.0}] $cy $family center "#d8f3ff" {Helvetica 8 bold}
        }
    }
}

proc parse_gate_netlist {gate_verilog} {
    set inputs {}
    set outputs {}
    set signals {}
    set cells {}
    foreach raw_stmt [split_verilog_statements $gate_verilog] {
        set stmt [string trim $raw_stmt]
        if {$stmt eq ""} { continue }
        set stmt [regsub {;$} $stmt ""]
        set first_token [lindex [split $stmt " \t\r\n"] 0]
        if {$first_token in {module endmodule assign always initial generate endgenerate}} {
            continue
        }
        if {[regexp {^input\b(.*)$} $stmt -> decl]} {
            foreach sig [parse_decl_signals $decl] {
                if {[lsearch -exact $inputs $sig] < 0} { lappend inputs $sig }
                if {[lsearch -exact $signals $sig] < 0} { lappend signals $sig }
            }
            continue
        }
        if {[regexp {^output\b(.*)$} $stmt -> decl]} {
            foreach sig [parse_decl_signals $decl] {
                if {[lsearch -exact $outputs $sig] < 0} { lappend outputs $sig }
                if {[lsearch -exact $signals $sig] < 0} { lappend signals $sig }
            }
            continue
        }
        if {[regexp {^wire\b(.*)$} $stmt -> decl]} {
            foreach sig [parse_decl_signals $decl] {
                if {[lsearch -exact $signals $sig] < 0} { lappend signals $sig }
            }
            continue
        }
        if {[regexp {^(\w+)\s+(\w+)\s*\((.*)\)$} $stmt -> cell_type inst_name pin_str]} {
            set pins {}
            set input_pins {}
            set output_pins {}
            foreach pin_clause [split_pins $pin_str] {
                if {[regexp {\.(\w+)\((.*?)\)} $pin_clause -> pin_name signal_name]} {
                    set signal_name [string trim $signal_name]
                    if {[lsearch -exact $signals $signal_name] < 0} { lappend signals $signal_name }
                    set role [gate_pin_role $pin_name]
                    set pin_record [dict create name $pin_name signal $signal_name role $role]
                    lappend pins $pin_record
                    if {$role eq "out"} {
                        lappend output_pins $pin_record
                    } else {
                        lappend input_pins $pin_record
                    }
                }
            }
            lappend cells [dict create type $cell_type inst $inst_name pins $pins inputs $input_pins outputs $output_pins]
        }
    }
    return [dict create inputs $inputs outputs $outputs signals $signals cells $cells]
}

proc gate_resolve_outputs {outputs signals} {
    set resolved {}
    foreach out $outputs {
        set matched 0
        foreach sig $signals {
            if {[string match "${out}\[*" $sig]} {
                lappend resolved $sig
                set matched 1
            }
        }
        if {!$matched} { lappend resolved $out }
    }
    return [lsort -unique $resolved]
}

proc gate_compute_levels {netlist} {
    set driver_map [dict create]
    set cell_type_map [dict create]
    foreach cell [dict get $netlist cells] {
        dict set cell_type_map [dict get $cell inst] [dict get $cell type]
        foreach pin [dict get $cell outputs] {
            dict set driver_map [dict get $pin signal] [dict get $cell inst]
        }
    }

    set levels [dict create]
    foreach cell [dict get $netlist cells] {
        dict set levels [dict get $cell inst] 0
    }

    set cell_total [llength [dict get $netlist cells]]
    set limit [expr {$cell_total * 4 + 4}]
    for {set pass 0} {$pass < $limit} {incr pass} {
        set changed 0
        foreach cell [dict get $netlist cells] {
            set inst [dict get $cell inst]
            set cell_type [dict get $cell type]
            if {[gate_is_sequential $cell_type]} {
                set best 1
            } else {
                set best 0
            }
            foreach pin [dict get $cell inputs] {
                set sig [dict get $pin signal]
                if {[dict exists $driver_map $sig]} {
                    set src [dict get $driver_map $sig]
                    if {$src ne $inst} {
                        if {[dict exists $cell_type_map $src] && [gate_is_sequential [dict get $cell_type_map $src]]} {
                            set candidate 1
                        } else {
                            set src_level [dict get $levels $src]
                            set candidate [expr {$src_level + 1}]
                        }
                        if {$candidate > $best} { set best $candidate }
                    }
                } elseif {[lsearch -exact [dict get $netlist inputs] $sig] >= 0 || [gate_is_constant $sig]} {
                    if {1 > $best} { set best 1 }
                }
            }
            if {$best != [dict get $levels $inst]} {
                dict set levels $inst $best
                set changed 1
            }
        }
        if {!$changed} { break }
    }
    return [list $levels $driver_map]
}

proc gate_cell_sort_pair_compare {a b} {
    set sa [lindex $a 0]
    set sb [lindex $b 0]
    if {$sa < $sb} { return -1 }
    if {$sa > $sb} { return 1 }
    return [string compare [lindex $a 1] [lindex $b 1]]
}

proc gate_cell_barycenter {cell driver_map input_rows row_map} {
    set total 0.0
    set count 0
    foreach pin [dict get $cell inputs] {
        set sig [dict get $pin signal]
        if {[dict exists $driver_map $sig]} {
            set src [dict get $driver_map $sig]
            if {[dict exists $row_map $src]} {
                set total [expr {$total + [dict get $row_map $src]}]
                incr count
            }
        } elseif {[dict exists $input_rows $sig]} {
            set total [expr {$total + [dict get $input_rows $sig]}]
            incr count
        }
    }
    if {$count == 0} {
        return 1000000.0
    }
    expr {$total / double($count)}
}

proc gate_sort_cells_by_barycenter {cells driver_map input_rows row_map} {
    set keyed {}
    foreach cell $cells {
        lappend keyed [list \
            [gate_cell_barycenter $cell $driver_map $input_rows $row_map] \
            [dict get $cell inst] \
            $cell]
    }
    set sorted {}
    foreach pair [lsort -command gate_cell_sort_pair_compare $keyed] {
        lappend sorted [lindex $pair 2]
    }
    return $sorted
}

proc gate_pin_anchor {box side index total} {
    set x [dict get $box x]
    set y [dict get $box y]
    set w [dict get $box w]
    set h [dict get $box h]
    if {$total < 1} { set total 1 }
    set step [expr {$h / double($total + 1)}]
    set py [expr {$y + ($index + 1) * $step}]
    if {$side eq "left"} {
        return [list $x $py]
    }
    return [list [expr {$x + $w}] $py]
}

proc gate_pin_geometry {box pin side index total unit} {
    set x [dict get $box x]
    set y [dict get $box y]
    set w [dict get $box w]
    set h [dict get $box h]
    set family [expr {[dict exists $box family] ? [dict get $box family] : "box"}]
    set sx1 [expr {$x + 26 * $unit}]
    set sx2 [expr {$x + $w - 26 * $unit}]
    set sy1 [expr {$y + 18 * $unit}]
    set sy2 [expr {$y + $h - 18 * $unit}]
    set cy [expr {($sy1 + $sy2) / 2.0}]
    set pin_name [string toupper [dict get $pin name]]
    set role [dict get $pin role]

    if {$family eq "dff"} {
        if {$side eq "left"} {
            set px $x
            set ix [expr {$sx1 + 6.0 * $unit}]
            if {$role eq "reset"} {
                set py [expr {$sy1 + 10.0 * $unit}]
            } elseif {$role eq "clock"} {
                set py [expr {$sy2 - 10.0 * $unit}]
            } elseif {$pin_name in {D DI DATA}} {
                set py $cy
            } else {
                set py [expr {$cy + ($index - max(0, ($total - 1) / 2.0)) * 12.0 * $unit}]
            }
        } else {
            set px [expr {$x + $w}]
            set ix [expr {$sx2 - 4.0 * $unit}]
            if {$pin_name in {QN ZN QB QBAR}} {
                set py [expr {$cy + 12.0 * $unit}]
            } elseif {$total > 1 && $index > 0} {
                set py [expr {$cy - 12.0 * $unit + $index * 12.0 * $unit}]
            } else {
                set py $cy
            }
        }
        return [list $px $py $ix $py]
    }

    if {$total < 1} { set total 1 }
    set span_y1 [expr {$sy1 + 4.0 * $unit}]
    set span_y2 [expr {$sy2 - 4.0 * $unit}]
    set step [expr {($span_y2 - $span_y1) / double($total + 1)}]
    set py [expr {$span_y1 + ($index + 1) * $step}]
    if {$side eq "left"} {
        return [list $x $py [expr {$sx1 - 2.0 * $unit}] $py]
    }
    return [list [expr {$x + $w}] $py [expr {$sx2 + 2.0 * $unit}] $py]
}

proc gate_pin_label_geometry {pin side px py pin_ix pin_iy unit} {
    set role [dict get $pin role]
    if {$side eq "left"} {
        set anchor w
        set lx [expr {$pin_ix + 6.0 * $unit}]
        if {$role eq "clock"} {
            set ly [expr {$pin_iy - 7.0 * $unit}]
        } elseif {$role eq "reset"} {
            set ly [expr {$pin_iy - 7.0 * $unit}]
        } else {
            set ly [expr {$pin_iy - 6.0 * $unit}]
        }
    } else {
        set anchor e
        set lx [expr {$pin_ix - 6.0 * $unit}]
        set ly [expr {$pin_iy - 6.0 * $unit}]
    }
    return [list $lx $ly $anchor]
}

proc gate_draw_pin_detail {canvas box pin side index total unit} {
    set color [gate_wire_color [dict get $pin role] [dict get $pin signal]]
    lassign [gate_pin_geometry $box $pin $side $index $total $unit] px py pin_ix pin_iy
    if {$side eq "left"} {
        set outer_x [expr {$px - 10.0 * $unit}]
    } else {
        set outer_x [expr {$px + 10.0 * $unit}]
    }
    set outer_y $py
    $canvas create line $outer_x $outer_y $px $py -fill $color -width 1.2
    $canvas create line $px $py $pin_ix $pin_iy -fill "#7a8fa6" -width 1.0
    lassign [gate_pin_label_geometry $pin $side $px $py $pin_ix $pin_iy $unit] label_x label_y label_anchor
    gate_draw_text_with_bg $canvas $label_x $label_y [dict get $pin name] $label_anchor $::C_DIM {Helvetica 7}
    return [dict create \
        edge_x $px edge_y $py \
        inner_x $pin_ix inner_y $pin_iy \
        outer_x $outer_x outer_y $outer_y]
}

proc gate_rect_stub_geometry {x y w h side unit} {
    set cy [expr {$y + $h / 2.0}]
    if {$side eq "left"} {
        return [dict create edge_x $x edge_y $cy tip_x [expr {$x - 10.0 * $unit}] tip_y $cy]
    }
    return [dict create edge_x [expr {$x + $w}] edge_y $cy tip_x [expr {$x + $w + 10.0 * $unit}] tip_y $cy]
}

proc gate_draw_rect_stub {canvas x y w h side color unit} {
    set geom [gate_rect_stub_geometry $x $y $w $h $side $unit]
    $canvas create line [dict get $geom edge_x] [dict get $geom edge_y] \
        [dict get $geom tip_x] [dict get $geom tip_y] \
        -fill $color -width 1.2
    return $geom
}

proc gate_pin_inner_anchor {box side index total unit} {
    lassign [gate_pin_anchor $box $side $index $total] px py
    if {$side eq "left"} {
        return [list [expr {$px + 18.0 * $unit}] $py]
    }
    return [list [expr {$px - 18.0 * $unit}] $py]
}

proc gate_pick_route_y {top_start bottom_start track_step lane_idx sy ty} {
    set slot [expr {int($lane_idx / 2) % 4}]
    set top_y [expr {$top_start + $slot * $track_step}]
    set bottom_y [expr {$bottom_start - $slot * $track_step}]
    set top_cost [expr {abs($sy - $top_y) + abs($ty - $top_y)}]
    set bottom_cost [expr {abs($sy - $bottom_y) + abs($ty - $bottom_y)}]
    set prefer_top [expr {$top_cost <= $bottom_cost}]
    if {$lane_idx % 2 == 1} {
        set prefer_top [expr {!$prefer_top}]
    }
    if {$prefer_top} {
        return $top_y
    }
    return $bottom_y
}

proc gate_wire_color {role signal_name} {
    if {$role eq "clock"} { return $::C_WARN }
    if {$role eq "reset"} { return $::C_ERR }
    if {[gate_is_constant $signal_name]} { return "#b0bec5" }
    return $::C_HIGHLIGHT
}

proc gate_find_matching_pin_index {pins signal_name} {
    set idx 0
    foreach pin $pins {
        if {[dict get $pin signal] eq $signal_name} {
            return $idx
        }
        incr idx
    }
    return -1
}

proc gate_find_matching_output_index {cell signal_name} {
    gate_find_matching_pin_index [dict get $cell outputs] $signal_name
}

proc gate_find_matching_input_index {cell signal_name} {
    gate_find_matching_pin_index [dict get $cell inputs] $signal_name
}

proc render_gate_netlist {} {
    set canvas .toparea.right.pages.synth.gate_canvas
    if {![winfo exists $canvas]} { return }
    if {$::synth_gate_netlist eq ""} {
        canvas_message $canvas "No gate netlist" "Run synthesis or open a finished project."
        return
    }

    if {$::gate_parsed_netlist eq ""} {
        set ::gate_parsed_netlist [parse_gate_netlist $::synth_gate_netlist]
    }
    set netlist $::gate_parsed_netlist
    set inputs [dict get $netlist inputs]
    set outputs [gate_resolve_outputs [dict get $netlist outputs] [dict get $netlist signals]]
    set cells [dict get $netlist cells]
    if {[llength $cells] == 0} {
        canvas_message $canvas "Synthesis result is empty" "No standard-cell instances were parsed from the gate netlist."
        return
    }

    lassign [gate_compute_levels $netlist] levels driver_map
    set columns [dict create]
    set max_level 0
    foreach cell $cells {
        set level [dict get $levels [dict get $cell inst]]
        dict lappend columns $level $cell
        if {$level > $max_level} { set max_level $level }
    }
    set level_count [expr {$max_level + 1}]
    if {$level_count < 1} { set level_count 1 }
    set fanout_map [gate_build_fanout_map $netlist $outputs]
    set constant_signals {}
    foreach cell $cells {
        foreach pin [dict get $cell inputs] {
            set sig [dict get $pin signal]
            if {[gate_is_constant $sig] && [lsearch -exact $constant_signals $sig] < 0} {
                lappend constant_signals $sig
            }
        }
    }
    set input_rows [dict create]
    set source_row_seed 0
    foreach sig $inputs {
        dict set input_rows $sig $source_row_seed
        incr source_row_seed
    }
    foreach sig $constant_signals {
        dict set input_rows $sig $source_row_seed
        incr source_row_seed
    }
    set row_map [dict create]

    $canvas delete all
    if {$::gate_zoom < 0.55} {
        set unit 0.55
    } else {
        set unit $::gate_zoom
    }

    set view_w [winfo width $canvas]
    if {$view_w < 760} { set view_w 1080 }
    set view_h [winfo height $canvas]
    if {$view_h < 420} { set view_h 720 }

    set cell_w [expr {136.0 * $unit}]
    set base_h [expr {56.0 * $unit}]
    set input_w [expr {104.0 * $unit}]
    set output_w [expr {118.0 * $unit}]
    set module_pad_x [expr {26.0 * $unit}]
    set module_pad_y [expr {22.0 * $unit}]
    set title_h [expr {34.0 * $unit}]
    set content_gap_x [expr {38.0 * $unit}]
    set row_pitch [expr {92.0 * $unit}]
    set column_gap_y [expr {28.0 * $unit}]
    set min_col_pitch [expr {$cell_w + 34.0 * $unit}]
    set level_divisor $level_count
    if {$level_divisor < 1} { set level_divisor 1 }
    set fit_col_pitch [expr {($view_w - 180.0 * $unit - $input_w - $output_w - $cell_w) / double($level_divisor)}]
    set x_step [gate_clamp $fit_col_pitch [expr {$cell_w + 28.0 * $unit}] [expr {156.0 * $unit}]]

    set column_layouts [dict create]
    set max_column_h [expr {$base_h + 12.0 * $unit}]
    for {set level 0} {$level <= $max_level} {incr level} {
        if {[dict exists $columns $level]} {
            set column_cells [gate_sort_cells_by_barycenter [dict get $columns $level] $driver_map $input_rows $row_map]
            set entries {}
            set total_h 0.0
            foreach cell $column_cells {
                set cell_h [gate_estimate_cell_height $cell $unit]
                if {[llength $entries] > 0} {
                    set total_h [expr {$total_h + $column_gap_y}]
                }
                set total_h [expr {$total_h + $cell_h}]
                lappend entries [list $cell $cell_h]
            }
            dict set column_layouts $level [dict create entries $entries total_h $total_h]
            if {$total_h > $max_column_h} { set max_column_h $total_h }
        }
    }
    set source_count [expr {[llength $inputs] + [llength $constant_signals]}]
    if {$source_count < 1} { set source_count 1 }
    set source_region_h [expr {30.0 * $unit + ($source_count - 1) * $row_pitch}]
    set output_count [expr {[llength $outputs] > 0 ? [llength $outputs] : 1}]
    set output_region_h [expr {30.0 * $unit + ($output_count - 1) * $row_pitch}]
    set cell_region_h [expr {max($max_column_h, $source_region_h, $output_region_h) + 24.0 * $unit}]
    set route_band_h [expr {54.0 * $unit}]
    set inner_h [expr {$title_h + $route_band_h + $cell_region_h + $route_band_h + 46.0 * $unit}]
    set cell_x0 [expr {$module_pad_x + $input_w + $content_gap_x}]
    set output_x_local [expr {$cell_x0 + $max_level * $x_step + $cell_w + $content_gap_x}]
    set module_w [expr {$output_x_local + $output_w + $module_pad_x}]
    set module_h [expr {$inner_h + $module_pad_y * 2}]
    set module_x [expr {$module_w < $view_w ? ($view_w - $module_w) / 2.0 : 20.0 * $unit}]
    set module_y [expr {20.0 * $unit}]
    set canvas_w [expr {max($view_w, $module_x + $module_w + 20.0 * $unit)}]
    set canvas_h [expr {max($view_h, $module_y + $module_h + 20.0 * $unit)}]
    set title_y [expr {$module_y + 18.0 * $unit}]
    set route_top_y0 [expr {$module_y + $module_pad_y + $title_h}]
    set cell_top_y [expr {$route_top_y0 + $route_band_h}]
    set route_bottom_y0 [expr {$cell_top_y + $cell_region_h}]
    set route_track_step [expr {11.0 * $unit}]
    set route_top_track_start [expr {$route_top_y0 + 16.0 * $unit}]
    set route_bottom_track_start [expr {$route_bottom_y0 + $route_band_h - 16.0 * $unit}]
    if {$::current_module eq ""} {
        set display_module "top"
    } else {
        set display_module $::current_module
    }

    $canvas create rectangle $module_x $module_y [expr {$module_x + $module_w}] [expr {$module_y + $module_h}] \
        -fill "#1b2030" -outline $::C_BORDER -width 1.4
    $canvas create line $module_x [expr {$module_y + $title_h + 8.0 * $unit}] [expr {$module_x + $module_w}] [expr {$module_y + $title_h + 8.0 * $unit}] \
        -fill "#324156" -width 1.0
    $canvas create text [expr {$module_x + $module_w / 2.0}] $title_y \
        -text "Native Standard-Cell Schematic" -fill $::C_HIGHLIGHT -font {Helvetica 12 bold}
    gate_draw_text_with_bg $canvas [expr {$module_x + 12.0 * $unit}] [expr {$module_y + 16.0 * $unit}] \
        "module $display_module" nw $::C_DIM {Helvetica 8}
    $canvas create rectangle [expr {$module_x + 8.0 * $unit}] $route_top_y0 \
        [expr {$module_x + $module_w - 8.0 * $unit}] [expr {$route_top_y0 + $route_band_h}] \
        -fill "#172233" -outline "#26364b" -width 1.0
    $canvas create rectangle [expr {$module_x + 8.0 * $unit}] $route_bottom_y0 \
        [expr {$module_x + $module_w - 8.0 * $unit}] [expr {$route_bottom_y0 + $route_band_h}] \
        -fill "#172233" -outline "#26364b" -width 1.0
    gate_draw_text_with_bg $canvas [expr {$module_x + 14.0 * $unit}] [expr {$route_top_y0 + 10.0 * $unit}] \
        "top routing tracks" nw "#607d9c" {Helvetica 7}
    gate_draw_text_with_bg $canvas [expr {$module_x + 14.0 * $unit}] [expr {$route_bottom_y0 + 10.0 * $unit}] \
        "bottom routing tracks" nw "#607d9c" {Helvetica 7}

    set input_pos [dict create]
    set const_pos [dict create]
    set source_nodes {}
    foreach sig $inputs {
        lappend source_nodes [list input $sig]
    }
    foreach sig $constant_signals {
        lappend source_nodes [list const $sig]
    }
    set source_x [expr {$module_x + $module_pad_x}]
    for {set idx 0} {$idx < $source_count} {incr idx} {
        lassign [lindex $source_nodes $idx] kind sig
        if {$kind eq "input"} {
            set item_h [expr {30.0 * $unit}]
            set y [gate_centered_item_y $cell_top_y $cell_region_h $source_count $idx $row_pitch $item_h]
            set tip_x [expr {$source_x + 106.0 * $unit}]
            set tip_y [expr {$y + $item_h / 2.0}]
            dict set input_pos $sig [list $tip_x $tip_y]
            dict set input_rows $sig [expr {$y + $item_h / 2.0}]
            $canvas create rectangle $source_x $y [expr {$source_x + 96 * $unit}] [expr {$y + $item_h}] \
                -fill "#153a25" -outline $::C_OK -width 1.2
            $canvas create line [expr {$source_x + 96 * $unit}] $tip_y $tip_x $tip_y -fill $::C_OK -width 1.3
            gate_draw_text_with_bg $canvas [expr {$source_x + 48 * $unit}] [expr {$y + $item_h / 2.0}] $sig center $::C_OK {Helvetica 9 bold}
        } else {
            set item_h [expr {28.0 * $unit}]
            set y [gate_centered_item_y $cell_top_y $cell_region_h $source_count $idx $row_pitch $item_h]
            set tip_x [expr {$source_x + 106.0 * $unit}]
            set tip_y [expr {$y + $item_h / 2.0}]
            dict set const_pos $sig [list $tip_x $tip_y]
            dict set input_rows $sig [expr {$y + $item_h / 2.0}]
            $canvas create rectangle $source_x $y [expr {$source_x + 96 * $unit}] [expr {$y + $item_h}] \
                -fill "#2f323c" -outline "#b0bec5" -width 1.1
            $canvas create line [expr {$source_x + 96 * $unit}] $tip_y $tip_x $tip_y -fill "#b0bec5" -width 1.2
            gate_draw_text_with_bg $canvas [expr {$source_x + 48 * $unit}] [expr {$y + $item_h / 2.0}] $sig center "#cfd8dc" {Helvetica 8 bold}
        }
    }

    set cell_boxes [dict create]
    set signal_sources [dict create]
    set cell_by_inst [dict create]
    set input_pin_targets [dict create]
    for {set level 0} {$level <= $max_level} {incr level} {
        if {![dict exists $column_layouts $level]} { continue }
        set layout [dict get $column_layouts $level]
        set column_y [expr {$cell_top_y + ($cell_region_h - [dict get $layout total_h]) / 2.0}]
        set row 0
        foreach entry [dict get $layout entries] {
            lassign $entry cell cell_h
            set inst [dict get $cell inst]
            set cell_type [dict get $cell type]
            set x [expr {$module_x + $cell_x0 + $level * $x_step}]
            set y $column_y
            set column_y [expr {$column_y + $cell_h + $column_gap_y}]
            dict set row_map $inst [expr {$y + $cell_h / 2.0}]
            dict set cell_by_inst $inst $cell
            set box [dict create x $x y $y w $cell_w h $cell_h family [gate_cell_family $cell_type] type $cell_type inst $inst]
            dict set cell_boxes $inst $box
            gate_draw_cell $canvas $box $cell_type $inst $unit

            set input_index 0
            foreach pin [dict get $cell inputs] {
                set pin_geom [gate_draw_pin_detail $canvas $box $pin left $input_index [llength [dict get $cell inputs]] $unit]
                dict set input_pin_targets "${inst}:${input_index}" $pin_geom
                incr input_index
            }

            set output_index 0
            foreach pin [dict get $cell outputs] {
                set pin_geom [gate_draw_pin_detail $canvas $box $pin right $output_index [llength [dict get $cell outputs]] $unit]
                dict set signal_sources [dict get $pin signal] [dict create x [dict get $pin_geom outer_x] y [dict get $pin_geom outer_y] column $level inst $inst]
                incr output_index
            }
            incr row
        }
    }

    set output_pos [dict create]
    set output_x [expr {$module_x + $output_x_local}]
    set output_order {}
    foreach sig $outputs {
        if {[dict exists $signal_sources $sig]} {
            set source_info [dict get $signal_sources $sig]
            set source_row [dict get $source_info y]
        } elseif {[dict exists $input_rows $sig]} {
            set source_row [dict get $input_rows $sig]
        } else {
            set source_row 1000000
        }
        lappend output_order [list $source_row $sig]
    }
    set output_row 0
    foreach pair [lsort -real -index 0 $output_order] {
        set sig [lindex $pair 1]
        set item_h [expr {30.0 * $unit}]
        set y [gate_centered_item_y $cell_top_y $cell_region_h $output_count $output_row $row_pitch $item_h]
        set tip_x [expr {$output_x - 10.0 * $unit}]
        set tip_y [expr {$y + $item_h / 2.0}]
        dict set output_pos $sig [list $tip_x $tip_y]
        $canvas create rectangle $output_x $y [expr {$output_x + 110 * $unit}] [expr {$y + $item_h}] \
            -fill "#442026" -outline $::C_ERR -width 1.2
        $canvas create line $tip_x $tip_y $output_x $tip_y -fill "#ff9fa4" -width 1.3
        gate_draw_text_with_bg $canvas [expr {$output_x + 55 * $unit}] [expr {$y + $item_h / 2.0}] $sig center "#ff9fa4" {Helvetica 9 bold}
        incr output_row
    }

    set channel_counts [dict create]
    foreach cell $cells {
        set inst [dict get $cell inst]
        if {![dict exists $cell_boxes $inst]} { continue }
        set cell [dict get $cell_by_inst $inst]
        set box [dict get $cell_boxes $inst]
        set input_total [llength [dict get $cell inputs]]
        set input_index 0
        foreach pin [dict get $cell inputs] {
            set sig [dict get $pin signal]
            set role [dict get $pin role]
            if {[dict exists $input_pin_targets "${inst}:${input_index}"]} {
                set target_geom [dict get $input_pin_targets "${inst}:${input_index}"]
                set tx [dict get $target_geom outer_x]
                set ty [dict get $target_geom outer_y]
            } else {
                lassign [gate_pin_geometry $box $pin left $input_index $input_total $unit] edge_x ty pin_ix pin_iy
                set tx [expr {$edge_x - 10.0 * $unit}]
            }
            if {[dict exists $signal_sources $sig]} {
                set source_info [dict get $signal_sources $sig]
                set sx [dict get $source_info x]
                set sy [dict get $source_info y]
                set src_col [dict get $source_info column]
            } elseif {[dict exists $input_pos $sig]} {
                lassign [dict get $input_pos $sig] sx sy
                set src_col -1
            } elseif {[dict exists $const_pos $sig]} {
                lassign [dict get $const_pos $sig] sx sy
                set src_col -2
            } elseif {[gate_is_constant $sig]} {
                set sx [expr {$tx - 64 * $unit}]
                set sy $ty
                set src_col -2
                gate_draw_text_with_bg $canvas [expr {$sx - 8 * $unit}] $sy $sig e "#cfd8dc" {Helvetica 8}
            } else {
                set sx [expr {$tx - 60 * $unit}]
                set sy $ty
                set src_col -3
                gate_draw_text_with_bg $canvas [expr {$sx - 6 * $unit}] $sy $sig e $::C_DIM {Helvetica 7}
            }
            set dst_col [dict get $levels $inst]
            set channel_key "${src_col}_${dst_col}"
            if {[dict exists $channel_counts $channel_key]} {
                set lane_idx [dict get $channel_counts $channel_key]
            } else {
                set lane_idx 0
            }
            dict set channel_counts $channel_key [expr {$lane_idx + 1}]
            set route_y [gate_pick_route_y $route_top_track_start $route_bottom_track_start $route_track_step $lane_idx $sy $ty]
            set route_x1 [expr {$sx + 16.0 * $unit}]
            set route_x2 [expr {$tx - 16.0 * $unit}]
            if {$route_x2 <= $route_x1} {
                set route_x2 [expr {$tx - 8.0 * $unit}]
                if {$route_x2 <= $route_x1} {
                    set route_x1 [expr {$sx + 8.0 * $unit}]
                }
            }
            set color [gate_wire_color $role $sig]
            set line_label ""
            if {[gate_should_label_net $sig $fanout_map $inputs $outputs]} {
                set line_label $sig
            }
            gate_draw_connection $canvas $sx $sy $tx $ty $color 1.3 $line_label $::C_DIM $unit $route_x1 $route_x2 $route_y
            incr input_index
        }
    }

    foreach sig [dict keys $output_pos] {
        lassign [dict get $output_pos $sig] tx ty
        if {[dict exists $signal_sources $sig]} {
            set source_info [dict get $signal_sources $sig]
            set sx [dict get $source_info x]
            set sy [dict get $source_info y]
            set src_col [dict get $source_info column]
        } elseif {[dict exists $input_pos $sig]} {
            lassign [dict get $input_pos $sig] sx sy
            set src_col -1
        } else {
            continue
        }
        set dst_col [expr {$max_level + 1}]
        set channel_key "${src_col}_${dst_col}"
        if {[dict exists $channel_counts $channel_key]} {
            set lane_idx [dict get $channel_counts $channel_key]
        } else {
            set lane_idx 0
        }
        dict set channel_counts $channel_key [expr {$lane_idx + 1}]
        set route_y [gate_pick_route_y $route_top_track_start $route_bottom_track_start $route_track_step $lane_idx $sy $ty]
        set route_x1 [expr {$sx + 16.0 * $unit}]
        set route_x2 [expr {$tx - 16.0 * $unit}]
        if {$route_x2 <= $route_x1} {
            set route_x2 [expr {$tx - 8.0 * $unit}]
            if {$route_x2 <= $route_x1} {
                set route_x1 [expr {$sx + 8.0 * $unit}]
            }
        }
        gate_draw_connection $canvas $sx $sy $tx $ty $::C_HIGHLIGHT 1.4 $sig $::C_DIM $unit $route_x1 $route_x2 $route_y
    }

    $canvas configure -scrollregion "0 0 $canvas_w $canvas_h"
    if {[winfo exists .toparea.right.pages.synth.toolbar.status]} {
        .toparea.right.pages.synth.toolbar.status configure \
            -text "[llength $cells] cells | native schematic" -fg $::C_OK
    }
}

proc gate_zoom_in {} {
    set ::gate_zoom [expr {$::gate_zoom * 1.25}]
    if {$::gate_zoom > 4.0} { set ::gate_zoom 4.0 }
    render_gate_netlist
}

proc gate_zoom_out {} {
    set ::gate_zoom [expr {$::gate_zoom / 1.25}]
    if {$::gate_zoom < 0.45} { set ::gate_zoom 0.45 }
    render_gate_netlist
}

proc gate_zoom_reset {} {
    set ::gate_zoom 1.0
    render_gate_netlist
}

proc load_gate_from_project {} {
    if {$::current_module eq ""} { return }
    set gate_path [gui_state_text gate_path]
    if {$gate_path eq "" && $::current_project_dir ne ""} {
        set gate_path [file join $::current_project_dir "syn" "${::current_module}_synth_gate.v"]
    }
    if {![file exists $gate_path]} {
        set ::synth_gate_netlist ""
        set ::gate_parsed_netlist ""
        canvas_message .toparea.right.pages.synth.gate_canvas "No gate netlist" "Open a synthesized project or run synthesis first."
        return
    }
    debug "load_gate_from_project: $gate_path"
    set fp [open $gate_path r]
    set ::synth_gate_netlist [read $fp]
    close $fp
    set ::gate_parsed_netlist ""
    if {[winfo exists .toparea.right.pages.synth.gate_text]} {
        .toparea.right.pages.synth.gate_text delete 1.0 end
        .toparea.right.pages.synth.gate_text insert end $::synth_gate_netlist
    }
    render_gate_netlist
    console_log "  Native gate schematic loaded" "ok"
}

proc wave_dedup_changes {changes} {
    set dedup {}
    set prev_value "__none__"
    foreach item $changes {
        lassign $item t value
        if {$value eq $prev_value && [llength $dedup] > 0} {
            continue
        }
        lappend dedup [list $t $value]
        set prev_value $value
    }
    if {[llength $dedup] == 0} {
        return [list [list 0 x]]
    }
    return $dedup
}

proc parse_vcd_content {content} {
    set timescale "1ns"
    set max_time 0
    set scope {}
    array set signal_name {}
    array set signal_short {}
    array set signal_width {}
    array set signal_changes {}
    set current_time 0
    set header 1
    set timescale_capture 0
    set timescale_parts {}

    foreach raw_line [split $content "\n"] {
        set line [string trim $raw_line]
        if {$line eq ""} { continue }
        if {$header} {
            if {$timescale_capture} {
                if {$line ne "\$end"} {
                    foreach token [split $line " "] {
                        set token [string trim $token]
                        if {$token ne "" && $token ne "\$end"} {
                            lappend timescale_parts $token
                        }
                    }
                }
                if {[string first "\$end" $line] >= 0} {
                    if {[llength $timescale_parts] > 0} {
                        set timescale [join $timescale_parts ""]
                    }
                    set timescale_capture 0
                }
                continue
            }
            if {[string match "\$timescale*" $line]} {
                set payload [string trim [string range $line 10 end]]
                set payload [string map [list "\$end" ""] $payload]
                set payload [string trim $payload]
                if {$payload ne ""} {
                    set timescale [join [split $payload " "] ""]
                } else {
                    set timescale_capture 1
                    set timescale_parts {}
                }
                if {[string first "\$end" $line] < 0 && $payload eq ""} {
                    set timescale_capture 1
                }
                continue
            }
            if {[string match "\$scope*" $line]} {
                set tokens [split $line " "]
                if {[llength $tokens] >= 4} {
                    lappend scope [lindex $tokens 2]
                }
                continue
            }
            if {[string match "\$upscope*" $line]} {
                set scope [lrange $scope 0 end-1]
                continue
            }
            if {[string match "\$var*" $line]} {
                set tokens [split $line " "]
                if {[llength $tokens] >= 5} {
                    set width [lindex $tokens 2]
                    set code [lindex $tokens 3]
                    set short [lindex $tokens 4]
                    set fullname [join [concat $scope $short] "."]
                    set signal_name($code) $fullname
                    set signal_short($code) $short
                    set signal_width($code) $width
                    set signal_changes($code) [list [list 0 x]]
                }
                continue
            }
            if {[string match "\$enddefinitions*" $line]} {
                set header 0
            }
            continue
        }

        if {[string index $line 0] eq "#"} {
            set current_time [string range $line 1 end]
            if {$current_time > $max_time} { set max_time $current_time }
            continue
        }
        if {[string index $line 0] eq "b"} {
            if {[regexp {^b([01xXzZ]+)\s+(\S+)} $line -> value code] && [info exists signal_changes($code)]} {
                lappend signal_changes($code) [list $current_time [string tolower $value]]
            }
            continue
        }
        if {[regexp {^([01xXzZ])(\S+)$} $line -> value code] && [info exists signal_changes($code)]} {
            lappend signal_changes($code) [list $current_time [string tolower $value]]
        }
    }

    set signals {}
    set signal_map [dict create]
    foreach code [lsort [array names signal_name]] {
        set normalized [wave_dedup_changes $signal_changes($code)]
        if {[llength $normalized] > 0} {
            set last_change [lindex [lindex $normalized end] 0]
            if {$last_change > $max_time} { set max_time $last_change }
        }
        set record [dict create \
            code $code \
            name $signal_name($code) \
            short $signal_short($code) \
            width $signal_width($code) \
            changes $normalized]
        lappend signals $record
        dict set signal_map $signal_name($code) $record
    }
    return [dict create timescale $timescale max_time $max_time signals $signals signal_map $signal_map]
}

proc wave_select_default_signals {} {
    if {![winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} { return }
    .toparea.right.pages.sim.vpane.wave.main.ctrl.list selection clear 0 end
    set preferred {}
    foreach sig $::wave_signal_order {
        if {[string first "." $sig] < 0 && [regexp -nocase {(^|[._])clk($|[._\[])|clock} $sig]} {
            lappend preferred $sig
        }
    }
    foreach sig $::wave_signal_order {
        if {[string first "." $sig] < 0 && [regexp -nocase {rst|reset} $sig]} {
            lappend preferred $sig
        }
    }
    foreach sig $::wave_signal_order {
        if {[string first "." $sig] < 0 && [lsearch -exact $preferred $sig] < 0} {
            lappend preferred $sig
        }
    }
    foreach sig $::wave_signal_order {
        if {[lsearch -exact $preferred $sig] < 0} {
            lappend preferred $sig
        }
    }
    set limit [expr {[llength $preferred] < 8 ? [llength $preferred] : 8}]
    for {set i 0} {$i < $limit} {incr i} {
        set sig [lindex $preferred $i]
        set idx [lsearch -exact $::wave_signal_order $sig]
        if {$idx >= 0} {
            .toparea.right.pages.sim.vpane.wave.main.ctrl.list selection set $idx
        }
    }
    wave_apply_signal_selection
}

proc wave_apply_signal_selection {} {
    if {![winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} { return }
    set selected {}
    foreach idx [.toparea.right.pages.sim.vpane.wave.main.ctrl.list curselection] {
        lappend selected [.toparea.right.pages.sim.vpane.wave.main.ctrl.list get $idx]
    }
    set ::wave_visible_signals $selected
    render_waveform
}

proc wave_show_all {} {
    if {![winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} { return }
    .toparea.right.pages.sim.vpane.wave.main.ctrl.list selection set 0 end
    wave_apply_signal_selection
}

proc wave_clear_selection {} {
    if {![winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} { return }
    .toparea.right.pages.sim.vpane.wave.main.ctrl.list selection clear 0 end
    set ::wave_visible_signals {}
    render_waveform
}

proc wave_zoom_in {} {
    set ::wave_zoom [expr {$::wave_zoom * 1.25}]
    if {$::wave_zoom > 6.0} { set ::wave_zoom 6.0 }
    render_waveform
}

proc wave_zoom_out {} {
    set ::wave_zoom [expr {$::wave_zoom / 1.25}]
    if {$::wave_zoom < 0.4} { set ::wave_zoom 0.4 }
    render_waveform
}

proc wave_zoom_reset {} {
    set ::wave_zoom 1.0
    render_waveform
}

proc wave_format_value {value width} {
    if {$width <= 1} { return $value }
    if {[regexp {^[01]+$} $value]} {
        scan $value %b num
        return [format 0x%X $num]
    }
    return $value
}

proc wave_time_to_x {time left_gutter scale} {
    expr {$left_gutter + $time * $scale}
}

proc render_waveform {} {
    set canvas .toparea.right.pages.sim.vpane.wave.main.view.canvas
    if {![winfo exists $canvas]} { return }
    if {$::wave_data eq ""} {
        canvas_message $canvas "No waveform" "Run simulation to generate a persistent VCD waveform."
        return
    }
    if {[llength $::wave_visible_signals] == 0} {
        canvas_message $canvas "No signals selected" "Choose one or more signals in the waveform list."
        return
    }

    set max_time [dict get $::wave_data max_time]
    if {$max_time < 1} { set max_time 1 }
    set base_scale [expr {980.0 / double($max_time)}]
    if {$base_scale < 0.8} { set base_scale 0.8 }
    if {$base_scale > 26.0} { set base_scale 26.0 }
    set step_px [expr {$base_scale * $::wave_zoom}]
    set left_gutter 190
    set top_gutter 30
    set row_h 42
    set usable [llength $::wave_visible_signals]
    set width [expr {$left_gutter + ($max_time + 2) * $step_px}]
    set height [expr {$top_gutter + $usable * $row_h + 40}]

    $canvas delete all
    $canvas create rectangle 0 0 $width $height -fill $::C_INPUT_BG -outline $::C_INPUT_BG

    set tick_step [expr {int(ceil($max_time / (10.0 * $::wave_zoom)))}]
    if {$tick_step < 1} { set tick_step 1 }
    for {set t 0} {$t <= $max_time} {incr t $tick_step} {
        set x [wave_time_to_x $t $left_gutter $step_px]
        $canvas create line $x 0 $x $height -fill "#243448" -dash {.}
        $canvas create text $x 14 -text $t -fill $::C_DIM -font {Helvetica 8}
    }
    $canvas create text 12 14 -text "Signal" -fill $::C_HIGHLIGHT -anchor w -font {Helvetica 9 bold}
    $canvas create text $left_gutter 14 -text "Time ([dict get $::wave_data timescale])" -fill $::C_HIGHLIGHT -anchor w -font {Helvetica 9 bold}

    set row 0
    foreach sig_name $::wave_visible_signals {
        if {![dict exists $::wave_signal_map $sig_name]} { continue }
        set record [dict get $::wave_signal_map $sig_name]
        set width_bits [dict get $record width]
        set changes [dict get $record changes]
        set y0 [expr {$top_gutter + $row * $row_h}]
        set y_high [expr {$y0 + 8}]
        set y_low [expr {$y0 + 26}]
        set y_mid [expr {$y0 + 17}]
        $canvas create text 12 $y_mid -text $sig_name -anchor w -fill $::C_TEXT -font {Helvetica 8}
        $canvas create line 0 [expr {$y0 + $row_h - 4}] $width [expr {$y0 + $row_h - 4}] -fill "#223040"

        if {$width_bits > 1} {
            for {set i 0} {$i < [llength $changes]} {incr i} {
                lassign [lindex $changes $i] t value
                if {$i + 1 < [llength $changes]} {
                    set next_t [lindex [lindex $changes [expr {$i + 1}]] 0]
                } else {
                    set next_t [expr {$max_time + 1}]
                }
                set x1 [wave_time_to_x $t $left_gutter $step_px]
                set x2 [wave_time_to_x $next_t $left_gutter $step_px]
                if {$x2 <= $x1} { continue }
                $canvas create rectangle $x1 [expr {$y0 + 8}] $x2 [expr {$y0 + 28}] \
                    -fill "#1d3744" -outline "#4dd0e1"
                $canvas create text [expr {($x1 + $x2) / 2.0}] [expr {$y0 + 18}] \
                    -text [wave_format_value $value $width_bits] -fill "#b2ebf2" -font {Helvetica 7}
            }
        } else {
            set prev_level x
            set prev_x $left_gutter
            for {set i 0} {$i < [llength $changes]} {incr i} {
                lassign [lindex $changes $i] t value
                if {$i + 1 < [llength $changes]} {
                    set next_t [lindex [lindex $changes [expr {$i + 1}]] 0]
                } else {
                    set next_t [expr {$max_time + 1}]
                }
                set x1 [wave_time_to_x $t $left_gutter $step_px]
                set x2 [wave_time_to_x $next_t $left_gutter $step_px]
                switch -- $value {
                    1 { set y $y_high; set color $::C_OK }
                    0 { set y $y_low; set color $::C_WARN }
                    default { set y $y_mid; set color "#b0bec5" }
                }
                if {$i > 0 && $prev_level ne $value} {
                    switch -- $prev_level {
                        1 { set prev_y $y_high }
                        0 { set prev_y $y_low }
                        default { set prev_y $y_mid }
                    }
                    $canvas create line $x1 $prev_y $x1 $y -fill $color -width 1.2
                }
                $canvas create line $x1 $y $x2 $y -fill $color -width 2.2
                set prev_level $value
                set prev_x $x2
            }
        }
        incr row
    }
    $canvas configure -scrollregion "0 0 $width $height"
}

proc load_waveform_from_state {} {
    set wave_path [gui_state_text sim_vcd_path]
    if {$wave_path eq "" && $::current_project_dir ne "" && $::current_module ne ""} {
        set wave_path [file join $::current_project_dir "sim" "${::current_module}_tb.vcd"]
    }
    if {$wave_path eq "" || ![file exists $wave_path]} {
        set ::wave_data ""
        set ::wave_signal_map ""
        set ::wave_signal_order {}
        if {[winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} {
            .toparea.right.pages.sim.vpane.wave.main.ctrl.list delete 0 end
        }
        render_waveform
        return
    }
    set fp [open $wave_path r]
    set content [read $fp]
    close $fp
    set ::wave_data [parse_vcd_content $content]
    set ::wave_signal_map [dict get $::wave_data signal_map]
    set ::wave_signal_order {}
    if {[winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} {
        .toparea.right.pages.sim.vpane.wave.main.ctrl.list delete 0 end
    }
    foreach signal_record [dict get $::wave_data signals] {
        set sig_name [dict get $signal_record name]
        lappend ::wave_signal_order $sig_name
        if {[winfo exists .toparea.right.pages.sim.vpane.wave.main.ctrl.list]} {
            .toparea.right.pages.sim.vpane.wave.main.ctrl.list insert end $sig_name
        }
    }
    set ::wave_max_time [dict get $::wave_data max_time]
    wave_select_default_signals
}

proc hierarchy_parent_path {path} {
    set idx [string last "/" $path]
    if {$idx < 0} { return "" }
    return [string range $path 0 [expr {$idx - 1}]]
}

proc load_hierarchy_from_state {} {
    set path [gui_state_text exchange_hierarchy_path]
    if {$path eq "" || ![file exists $path]} {
        set ::hierarchy_rows {}
        render_hierarchy_treemap
        return
    }
    set fp [open $path r]
    set content [read $fp]
    close $fp
    set rows {}
    foreach line [split $content "\n"] {
        set line [string trim $line]
        if {$line eq "" || [string match "depth*" $line]} { continue }
        set cols [split $line "\t"]
        if {[llength $cols] < 6} { continue }
        lappend rows [dict create \
            depth [lindex $cols 0] \
            path [lindex $cols 1] \
            module [lindex $cols 2] \
            area_ge [lindex $cols 3] \
            cells [lindex $cols 4] \
            weight [lindex $cols 5]]
    }
    set ::hierarchy_rows $rows
    render_hierarchy_treemap
}

proc formal_parse_points_text {content} {
    set parsed [dict create interface_in {} interface_out {} interface_io {} outputs {} sequential {} methods {}]
    set section ""
    foreach raw_line [split $content "\n"] {
        set line [string trim $raw_line]
        if {$line eq ""} { continue }
        if {[string match "*:" $line]} {
            set section [string trimright $line ":"]
            continue
        }
        if {[regexp {^(IN|OUT|IO)\s+(\S+)\s+(.+)$} $line -> dir name desc]} {
            switch -- $dir {
                IN { dict lappend parsed interface_in [dict create name $name desc $desc] }
                OUT { dict lappend parsed interface_out [dict create name $name desc $desc] }
                IO { dict lappend parsed interface_io [dict create name $name desc $desc] }
            }
            continue
        }
        if {[string match "- *" $line]} {
            set item [string trim [string range $line 1 end]]
            switch -- $section {
                "Output Equivalence Points" { dict lappend parsed outputs $item }
                "Sequential Observation Points" { dict lappend parsed sequential $item }
                "Verification Method" { dict lappend parsed methods $item }
            }
        }
    }
    return $parsed
}

proc load_formal_points_from_state {} {
    set path [gui_state_text exchange_formal_points_path]
    set ::formal_points_data [load_text_file $path]
    render_formal_points_graph
}

proc render_formal_points_graph {} {
    set canvas .toparea.right.pages.formal.points_canvas
    if {![winfo exists $canvas]} { return }
    if {$::formal_points_data eq ""} {
        canvas_message $canvas "No equivalence graph" "Run formal verification to populate verified interface and observation points."
        return
    }

    set parsed [formal_parse_points_text $::formal_points_data]
    set width [winfo width $canvas]
    if {$width < 360} { set width 520 }
    set height [winfo height $canvas]
    if {$height < 260} { set height 420 }
    $canvas delete all
    $canvas create rectangle 0 0 $width $height -fill $::C_INPUT_BG -outline $::C_INPUT_BG

    set core_x [expr {$width / 2.0}]
    set core_y [expr {$height / 2.0 - 30}]
    set core_w 140
    set core_h 72
    set core_fill [expr {($::formal_result eq "PASS" || $::formal_result eq "EQUIVALENT") ? "#173b2a" : "#472024"}]
    set core_outline [expr {($::formal_result eq "PASS" || $::formal_result eq "EQUIVALENT") ? $::C_OK : $::C_ERR}]
    $canvas create rectangle [expr {$core_x - $core_w / 2.0}] [expr {$core_y - $core_h / 2.0}] \
        [expr {$core_x + $core_w / 2.0}] [expr {$core_y + $core_h / 2.0}] \
        -fill $core_fill -outline $core_outline -width 1.6
    $canvas create text $core_x [expr {$core_y - 10}] -text "RTL  ⇄  Gate" -fill $::C_HIGHLIGHT -font {Helvetica 12 bold}
    $canvas create text $core_x [expr {$core_y + 14}] -text $::formal_result -fill $core_outline -font {Helvetica 10 bold}
    set core_left_stub [gate_draw_rect_stub $canvas [expr {$core_x - $core_w / 2.0}] [expr {$core_y - $core_h / 2.0}] $core_w $core_h left "#4fc3f7" 1.0]
    set core_right_stub [gate_draw_rect_stub $canvas [expr {$core_x - $core_w / 2.0}] [expr {$core_y - $core_h / 2.0}] $core_w $core_h right $::C_OK 1.0]

    set left_items {}
    foreach key {interface_in interface_out interface_io} {
        foreach item [dict get $parsed $key] {
            if {$key eq "interface_in"} {
                set title "IN"
            } elseif {$key eq "interface_out"} {
                set title "OUT"
            } else {
                set title "IO"
            }
            lappend left_items [format "%s  %s  %s" $title [dict get $item name] [dict get $item desc]]
        }
    }

    set left_count [expr {[llength $left_items] > 0 ? [llength $left_items] : 1}]
    set left_x 26
    for {set i 0} {$i < [llength $left_items]} {incr i} {
        set y [expr {40 + $i * 56}]
        set item [lindex $left_items $i]
        set tx [expr {$core_x - $core_w / 2.0 - 10}]
        set ty [expr {$core_y - 20 + ($i % 4) * 14}]
        set item_w [expr {214 - $left_x}]
        set item_h 34
        $canvas create rectangle $left_x $y 214 [expr {$y + $item_h}] -fill "#233041" -outline "#4fc3f7" -width 1.2
        $canvas create text [expr {$left_x + 8}] [expr {$y + 17}] -anchor w -text $item -fill $::C_TEXT -font {Helvetica 8}
        $canvas create line $tx $ty [expr {$tx + 10}] $ty -fill "#4fc3f7" -width 1.1
        set item_stub [gate_draw_rect_stub $canvas $left_x $y $item_w $item_h right "#4fc3f7" 1.0]
        gate_draw_connection $canvas [dict get $item_stub tip_x] [dict get $item_stub tip_y] $tx $ty \
            "#4fc3f7" 1.1 "" $::C_DIM 1.0 [expr {[dict get $item_stub tip_x] + 22 + ($i % 2) * 8}] [expr {$tx - 18 - ($i % 2) * 8}] [expr {min([dict get $item_stub tip_y], $ty) - 18 - ($i % 2) * 10}]
    }

    set right_top [dict get $parsed outputs]
    if {[llength $right_top] == 0} {
        set right_top [list "No output points"]
    }
    for {set i 0} {$i < [llength $right_top]} {incr i} {
        set y [expr {46 + $i * 48}]
        set text [lindex $right_top $i]
        set sx [expr {$core_x + $core_w / 2.0 + 10}]
        set sy [expr {$core_y - 20 + ($i % 4) * 12}]
        set item_x [expr {$width - 220}]
        set item_w 196
        set item_h 30
        $canvas create rectangle $item_x $y [expr {$item_x + $item_w}] [expr {$y + $item_h}] \
            -fill "#25384a" -outline $::C_OK -width 1.2
        $canvas create text [expr {$width - 212}] [expr {$y + 15}] -anchor w -text $text -fill $::C_TEXT -font {Helvetica 8}
        $canvas create line [expr {$sx - 10}] $sy $sx $sy -fill $::C_OK -width 1.1
        set item_stub [gate_draw_rect_stub $canvas $item_x $y $item_w $item_h left $::C_OK 1.0]
        gate_draw_connection $canvas $sx $sy [dict get $item_stub tip_x] [dict get $item_stub tip_y] \
            $::C_OK 1.1 "" $::C_DIM 1.0 [expr {$sx + 18 + ($i % 2) * 8}] [expr {[dict get $item_stub tip_x] - 18 - ($i % 2) * 8}] [expr {min($sy, [dict get $item_stub tip_y]) - 16 - ($i % 2) * 10}]
    }

    set seq_items [dict get $parsed sequential]
    if {[llength $seq_items] == 0} {
        set seq_items [list "No sequential observation points"]
    }
    for {set i 0} {$i < [llength $seq_items]} {incr i} {
        set y [expr {$core_y + 72 + $i * 44}]
        set text [lindex $seq_items $i]
        set sx [expr {$core_x + 34}]
        set sy [expr {$core_y + $core_h / 2.0 + 10}]
        set item_x [expr {$width - 220}]
        set item_w 196
        set item_h 28
        $canvas create rectangle $item_x $y [expr {$item_x + $item_w}] [expr {$y + $item_h}] \
            -fill "#332c1f" -outline $::C_WARN -width 1.1
        $canvas create text [expr {$width - 212}] [expr {$y + 14}] -anchor w -text $text -fill $::C_TEXT -font {Helvetica 8}
        $canvas create line [expr {$sx - 10}] $sy $sx $sy -fill $::C_WARN -width 1.0
        set item_stub [gate_draw_rect_stub $canvas $item_x $y $item_w $item_h left $::C_WARN 1.0]
        gate_draw_connection $canvas $sx $sy [dict get $item_stub tip_x] [dict get $item_stub tip_y] \
            $::C_WARN 1.0 "" $::C_DIM 1.0 [expr {$sx + 20 + ($i % 2) * 10}] [expr {[dict get $item_stub tip_x] - 16 - ($i % 2) * 8}] [expr {max($sy, [dict get $item_stub tip_y]) + 16 + ($i % 2) * 10}]
    }

    set methods [dict get $parsed methods]
    if {[llength $methods] > 0} {
        $canvas create text 28 [expr {$height - 92}] -anchor nw -text "Verification Method" -fill $::C_HIGHLIGHT -font {Helvetica 9 bold}
        set i 0
        foreach method $methods {
            $canvas create text 34 [expr {$height - 70 + $i * 18}] -anchor nw -text [format "• %s" $method] -fill $::C_DIM -font {Helvetica 8}
            incr i
        }
    }

    $canvas configure -scrollregion "0 0 $width $height"
}

proc hierarchy_render_node {canvas nodes children path x y w h depth vertical} {
    set record [dict get $nodes $path]
    set colors {#24445a #2f5d50 #5f4c2d #4d325c #3a5440}
    set color_count [llength $colors]
    set fill [lindex $colors [expr {$depth % $color_count}]]
    if {$depth == 0} {
        set outline $::C_HIGHLIGHT
    } else {
        set outline $::C_BORDER
    }
    $canvas create rectangle $x $y [expr {$x + $w}] [expr {$y + $h}] -fill $fill -outline $outline -width 1.4
    set label "[dict get $record module]\n[format %.2f [dict get $record area_ge]] GE"
    $canvas create text [expr {$x + 8}] [expr {$y + 8}] -text $label -anchor nw -fill $::C_TEXT -font {Helvetica 9 bold}

    if {![dict exists $children $path]} { return }
    set child_paths [dict get $children $path]
    if {[llength $child_paths] == 0 || $w < 80 || $h < 60} { return }

    set total 0.0
    foreach child_path $child_paths {
        set child_area [dict get [dict get $nodes $child_path] area_ge]
        set total [expr {$total + $child_area}]
    }
    if {$total <= 0} { return }

    set offset 0.0
    foreach child_path $child_paths {
        set child_area [dict get [dict get $nodes $child_path] area_ge]
        set share [expr {$child_area / $total}]
        if {$vertical} {
            set cw [expr {$w * $share}]
            hierarchy_render_node $canvas $nodes $children $child_path [expr {$x + $offset}] [expr {$y + 26}] $cw [expr {$h - 26}] [expr {$depth + 1}] 0
            set offset [expr {$offset + $cw}]
        } else {
            set ch [expr {($h - 26) * $share}]
            hierarchy_render_node $canvas $nodes $children $child_path [expr {$x + 8}] [expr {$y + 26 + $offset}] [expr {$w - 16}] $ch [expr {$depth + 1}] 1
            set offset [expr {$offset + $ch}]
        }
    }
}

proc render_hierarchy_treemap {} {
    set canvas .toparea.right.pages.summary.hpane.hier.canvas
    if {![winfo exists $canvas]} { return }
    if {[llength $::hierarchy_rows] == 0} {
        canvas_message $canvas "No hierarchy data" "Open a project with saved exchange state or run the full flow."
        return
    }

    set nodes [dict create]
    set children [dict create]
    set root ""
    foreach row $::hierarchy_rows {
        set path [dict get $row path]
        dict set nodes $path $row
        if {$root eq "" || [dict get $row depth] == 0} {
            set root $path
        }
        set parent [hierarchy_parent_path $path]
        if {$parent ne ""} {
            dict lappend children $parent $path
        }
    }

    $canvas delete all
    set width [winfo width $canvas]
    if {$width < 260} { set width 420 }
    set height [winfo height $canvas]
    if {$height < 180} { set height 260 }
    hierarchy_render_node $canvas $nodes $children $root 10 10 [expr {$width - 20}] [expr {$height - 20}] 0 1
    $canvas configure -scrollregion "0 0 $width $height"
}

proc timing_find_path {path_index} {
    foreach path $::timing_paths {
        if {[dict get $path index] == $path_index} {
            return $path
        }
    }
    return ""
}

proc timing_refresh_path_list {} {
    set listbox .toparea.right.pages.timing.vpane.graph.main.left.list
    if {![winfo exists $listbox]} { return }
    $listbox delete 0 end
    foreach path $::timing_paths {
        set idx [dict get $path index]
        if {[dict get $path available]} {
            set status [expr {[dict get $path met] ? "MET" : "VIO"}]
            set line [format "#%-2d %-18s -> %-18s %7.3f ns  %s" \
                $idx \
                [string range [dict get $path startpoint] 0 17] \
                [string range [dict get $path endpoint] 0 17] \
                [dict get $path delay_ns] \
                $status]
        } else {
            set line [format "#%-2d (unavailable)" $idx]
        }
        $listbox insert end $line
    }
    if {[$listbox size] == 0} { return }
    set sel [expr {$::timing_selected_path - 1}]
    if {$sel < 0 || $sel >= [$listbox size]} {
        set sel 0
        set ::timing_selected_path 1
    }
    $listbox selection clear 0 end
    $listbox selection set $sel
    $listbox see $sel
}

proc timing_select_path {} {
    set listbox .toparea.right.pages.timing.vpane.graph.main.left.list
    if {![winfo exists $listbox]} { return }
    set selected [$listbox curselection]
    if {[llength $selected] == 0} { return }
    set ::timing_selected_path [expr {[lindex $selected 0] + 1}]
    render_timing_path_graph
}

proc load_timing_paths_from_state {} {
    set path [gui_state_text exchange_timing_paths_path]
    set ::timing_paths {}
    if {$path eq "" || ![file exists $path]} {
        timing_refresh_path_list
        render_timing_path_graph
        return
    }

    set fp [open $path r]
    set content [read $fp]
    close $fp

    set path_map [dict create]
    foreach raw_line [split $content "\n"] {
        set line [string trim $raw_line]
        if {$line eq "" || [string match "path_index*" $line]} { continue }
        set cols [split $line "\t"]
        if {[llength $cols] < 13} { continue }
        set idx [lindex $cols 0]
        if {![dict exists $path_map $idx]} {
            dict set path_map $idx [dict create \
                index $idx \
                available [bool_value [lindex $cols 1]] \
                met [bool_value [lindex $cols 2]] \
                startpoint [lindex $cols 3] \
                endpoint [lindex $cols 4] \
                delay_ns [lindex $cols 5] \
                slack_ns [lindex $cols 6] \
                stage_count [lindex $cols 7] \
                stages {}]
        }
        if {[lindex $cols 8] ne "-1"} {
            set stages [dict get $path_map $idx stages]
            lappend stages [dict create \
                name [lindex $cols 9] \
                type [lindex $cols 10] \
                incr_delay_ns [lindex $cols 11] \
                cumul_delay_ns [lindex $cols 12]]
            dict set path_map $idx stages $stages
        }
    }

    set ordered {}
    foreach idx [lsort -integer [dict keys $path_map]] {
        lappend ordered [dict get $path_map $idx]
    }
    set ::timing_paths $ordered
    timing_refresh_path_list
    render_timing_path_graph
}

proc timing_zoom_in {} {
    set ::timing_zoom [expr {$::timing_zoom * 1.25}]
    if {$::timing_zoom > 4.0} { set ::timing_zoom 4.0 }
    render_timing_path_graph
}

proc timing_zoom_out {} {
    set ::timing_zoom [expr {$::timing_zoom / 1.25}]
    if {$::timing_zoom < 0.45} { set ::timing_zoom 0.45 }
    render_timing_path_graph
}

proc timing_zoom_reset {} {
    set ::timing_zoom 1.0
    render_timing_path_graph
}

proc render_timing_path_graph {} {
    set canvas .toparea.right.pages.timing.vpane.graph.main.right.canvas
    if {![winfo exists $canvas]} { return }
    if {$::synth_gate_netlist eq ""} {
        canvas_message $canvas "No gate netlist" "Run synthesis or timing analysis first."
        return
    }
    if {$::gate_parsed_netlist eq ""} {
        set ::gate_parsed_netlist [parse_gate_netlist $::synth_gate_netlist]
    }
    set path [timing_find_path $::timing_selected_path]
    if {$path eq ""} {
        canvas_message $canvas "No critical path data" "Run timing analysis to produce a top-10 path report."
        return
    }
    if {![dict get $path available]} {
        canvas_message $canvas "Path unavailable" "This slot is padded because the design has fewer real critical paths."
        return
    }

    set netlist $::gate_parsed_netlist
    set outputs_by_signal [dict create]
    foreach cell [dict get $netlist cells] {
        foreach pin [dict get $cell outputs] {
            dict set outputs_by_signal [dict get $pin signal] $cell
        }
    }

    set stages [dict get $path stages]
    if {[llength $stages] == 0} {
        canvas_message $canvas "No stage data" "The timing report did not expose per-stage critical-path details."
        return
    }

    if {$::timing_zoom < 0.55} {
        set unit 0.55
    } else {
        set unit $::timing_zoom
    }

    $canvas delete all
    set view_w [winfo width $canvas]
    if {$view_w < 720} { set view_w 1024 }
    set view_h [winfo height $canvas]
    if {$view_h < 360} { set view_h 620 }

    set stage_count [llength $stages]
    if {$stage_count < 1} { set stage_count 1 }
    set cell_w [expr {136.0 * $unit}]
    set base_h [expr {56.0 * $unit}]
    set io_w [expr {128.0 * $unit}]
    set module_pad_x [expr {26.0 * $unit}]
    set module_pad_y [expr {22.0 * $unit}]
    set title_h [expr {38.0 * $unit}]
    set stage_divisor $stage_count
    if {$stage_divisor < 1} { set stage_divisor 1 }
    set fit_x_step [expr {($view_w - 190.0 * $unit - 2.0 * $io_w - $cell_w) / double($stage_divisor)}]
    set x_step [gate_clamp $fit_x_step [expr {$cell_w + 28.0 * $unit}] [expr {160.0 * $unit}]]

    set local_start_x $module_pad_x
    set cell_x0 [expr {$local_start_x + $io_w + 42.0 * $unit}]
    set local_end_x [expr {$cell_x0 + ($stage_count - 1) * $x_step + $cell_w + 46.0 * $unit}]
    set module_w [expr {$local_end_x + $io_w + $module_pad_x}]
    set module_h [expr {$title_h + $module_pad_y * 2 + 250.0 * $unit}]
    set module_x [expr {$module_w < $view_w ? ($view_w - $module_w) / 2.0 : 20.0 * $unit}]
    set module_y [expr {20.0 * $unit}]
    set canvas_w [expr {max($view_w, $module_x + $module_w + 20.0 * $unit)}]
    set canvas_h [expr {max($view_h, $module_y + $module_h + 20.0 * $unit)}]
    set top_margin [expr {$module_y + $module_pad_y + $title_h}]
    set path_center_y [expr {$top_margin + 98.0 * $unit}]
    set io_y [expr {$path_center_y - 17.0 * $unit}]
    set trunk_y [expr {$top_margin + 28.0 * $unit}]
    set title_text [format "Critical Path #%d   delay %.3f ns   slack %.3f ns" \
        [dict get $path index] [dict get $path delay_ns] [dict get $path slack_ns]]

    $canvas create rectangle $module_x $module_y [expr {$module_x + $module_w}] [expr {$module_y + $module_h}] \
        -fill "#1b2030" -outline $::C_BORDER -width 1.4
    $canvas create line $module_x [expr {$module_y + $title_h + 8.0 * $unit}] [expr {$module_x + $module_w}] [expr {$module_y + $title_h + 8.0 * $unit}] \
        -fill "#324156" -width 1.0
    $canvas create text [expr {$module_x + $module_w / 2.0}] [expr {$module_y + 18.0 * $unit}] \
        -text $title_text -anchor c -fill $::C_HIGHLIGHT -font {Helvetica 11 bold}

    set signal_chain {}
    foreach stage $stages {
        lappend signal_chain [dict get $stage name]
    }
    if {[lindex $signal_chain 0] ne [dict get $path startpoint]} {
        set signal_chain [linsert $signal_chain 0 [dict get $path startpoint]]
    }

    set start_sig [dict get $path startpoint]
    set start_x [expr {$module_x + $local_start_x}]
    set start_w [expr {120.0 * $unit}]
    set start_h [expr {34.0 * $unit}]
    $canvas create rectangle $start_x $io_y [expr {$start_x + $start_w}] [expr {$io_y + $start_h}] \
        -fill "#153a25" -outline $::C_OK -width 1.3
    gate_draw_text_with_bg $canvas [expr {$start_x + 60 * $unit}] [expr {$io_y + 17 * $unit}] $start_sig center $::C_OK {Helvetica 9 bold}
    set start_stub [gate_draw_rect_stub $canvas $start_x $io_y $start_w $start_h right $::C_OK $unit]
    set signal_sources [dict create]
    dict set signal_sources $start_sig [dict create x [dict get $start_stub tip_x] y [dict get $start_stub tip_y]]

    set current_x [expr {$module_x + $cell_x0}]
    for {set i 1} {$i < [llength $signal_chain]} {incr i} {
        set signal_name [lindex $signal_chain $i]
        if {![dict exists $outputs_by_signal $signal_name]} {
            continue
        }
        set cell [dict get $outputs_by_signal $signal_name]
        set pin_count [llength [dict get $cell pins]]
        if {$pin_count < 2} { set pin_count 2 }
        set cell_h [expr {$base_h + $pin_count * 10 * $unit}]
        set box_y [expr {$path_center_y - $cell_h / 2.0}]
        set box [dict create x $current_x y $box_y w $cell_w h $cell_h family [gate_cell_family [dict get $cell type]] type [dict get $cell type] inst [dict get $cell inst]]
        gate_draw_cell $canvas $box [dict get $cell type] [dict get $cell inst] $unit
        set timing_input_targets [dict create]
        set timing_output_targets [dict create]
        set in_pin_index 0
        foreach cell_pin [dict get $cell inputs] {
            set pin_geom [gate_draw_pin_detail $canvas $box $cell_pin left $in_pin_index [llength [dict get $cell inputs]] $unit]
            dict set timing_input_targets $in_pin_index $pin_geom
            incr in_pin_index
        }
        set out_pin_index 0
        foreach cell_pin [dict get $cell outputs] {
            set pin_geom [gate_draw_pin_detail $canvas $box $cell_pin right $out_pin_index [llength [dict get $cell outputs]] $unit]
            dict set timing_output_targets $out_pin_index $pin_geom
            dict set signal_sources [dict get $cell_pin signal] [dict create x [dict get $pin_geom outer_x] y [dict get $pin_geom outer_y]]
            incr out_pin_index
        }

        set prev_signal [lindex $signal_chain [expr {$i - 1}]]
        set in_idx [gate_find_matching_input_index $cell $prev_signal]
        if {$in_idx < 0} { set in_idx 0 }
        set input_pin [lindex [dict get $cell inputs] $in_idx]
        if {[dict exists $timing_input_targets $in_idx]} {
            set target_geom [dict get $timing_input_targets $in_idx]
            set tx [dict get $target_geom outer_x]
            set ty [dict get $target_geom outer_y]
        } else {
            lassign [gate_pin_geometry $box $input_pin left $in_idx [llength [dict get $cell inputs]] $unit] edge_x ty pin_ix pin_iy
            set tx [expr {$edge_x - 10.0 * $unit}]
        }
        if {[dict exists $signal_sources $prev_signal]} {
            set source_info [dict get $signal_sources $prev_signal]
            set lane_y [expr {$trunk_y + (($i - 1) % 4) * 16.0 * $unit}]
            set route_x1 [expr {[dict get $source_info x] + 16.0 * $unit}]
            set route_x2 [expr {$tx - 16.0 * $unit}]
            if {$route_x2 <= $route_x1} {
                set route_x2 [expr {$tx - 8.0 * $unit}]
            }
            gate_draw_connection $canvas [dict get $source_info x] [dict get $source_info y] $tx $ty \
                [gate_wire_color [dict get $input_pin role] $prev_signal] 1.5 \
                $prev_signal $::C_DIM $unit $route_x1 $route_x2 $lane_y
        }

        set out_idx [gate_find_matching_output_index $cell $signal_name]
        if {$out_idx < 0} { set out_idx 0 }
        set stage_info [lindex $stages [expr {$i - 1}]]
        set delay_note [format "+%.3f / %.3f ns" [dict get $stage_info incr_delay_ns] [dict get $stage_info cumul_delay_ns]]
        gate_draw_text_with_bg $canvas [expr {$current_x + 4 * $unit}] [expr {$box_y + $cell_h + 12 * $unit}] \
            $delay_note nw $::C_WARN {Helvetica 8}
        set current_x [expr {$current_x + $x_step}]
    }

    set end_sig [dict get $path endpoint]
    set end_x [expr {$module_x + $local_end_x}]
    set end_w [expr {130.0 * $unit}]
    set end_h [expr {34.0 * $unit}]
    $canvas create rectangle $end_x $io_y [expr {$end_x + $end_w}] [expr {$io_y + $end_h}] \
        -fill "#442026" -outline $::C_ERR -width 1.3
    gate_draw_text_with_bg $canvas [expr {$end_x + 65 * $unit}] [expr {$io_y + 17 * $unit}] $end_sig center "#ff9fa4" {Helvetica 9 bold}
    set end_stub [gate_draw_rect_stub $canvas $end_x $io_y $end_w $end_h left $::C_ERR $unit]
    if {[dict exists $signal_sources $end_sig]} {
        set source_info [dict get $signal_sources $end_sig]
        set lane_y [expr {$trunk_y + ($stage_count % 4) * 16.0 * $unit}]
        set route_x1 [expr {[dict get $source_info x] + 16.0 * $unit}]
        set route_x2 [expr {[dict get $end_stub tip_x] - 16.0 * $unit}]
        if {$route_x2 <= $route_x1} {
            set route_x2 [expr {[dict get $end_stub tip_x] - 8.0 * $unit}]
        }
        gate_draw_connection $canvas [dict get $source_info x] [dict get $source_info y] [dict get $end_stub tip_x] [dict get $end_stub tip_y] \
            $::C_HIGHLIGHT 1.5 $end_sig $::C_DIM $unit $route_x1 $route_x2 $lane_y
    }

    $canvas configure -scrollregion "0 0 $canvas_w $canvas_h"
}

proc split_pins {pin_str} {
    # Split pin string like ".A(a), .B(b), .C(c)" into list
    set result {}
    set depth 0
    set current ""
    foreach ch [split $pin_str ""] {
        if {$ch eq "("} { incr depth; append current $ch
        } elseif {$ch eq ")"} { incr depth -1; append current $ch
        } elseif {$ch eq "," && $depth == 0} {
            set trimmed [string trim $current]
            if {$trimmed ne ""} { lappend result $trimmed }
            set current ""
        } else { append current $ch }
    }
    set trimmed [string trim $current]
    if {$trimmed ne ""} { lappend result $trimmed }
    return $result
}

# ===================== Console =====================
proc console_log {msg {type ""}} {
    if {![winfo exists .console.text]} { return }
    .console.text configure -state normal
    set ts [clock format [clock seconds] -format "%H:%M:%S"]
    set tag "${type}_tag"
    if {$tag eq "_tag"} { set tag "dim_tag" }
    .console.text insert end "\[$ts\] $msg\n" $tag
    .console.text see end
    .console.text configure -state disabled
    incr ::console_lines
}

proc console_cmd {} {
    set cmd [console_input_get]
    if {$cmd eq ""} return
    console_input_clear
    console_log "ai_digital ▸ $cmd" "cmd"
    debug "User command: $cmd"
    # Send to pipe subprocess
    if {$::pipe_id ne ""} {
        pipe_send $cmd
    } else {
        console_log "  CLI subprocess not connected" "err"
        # Try to reconnect
        pipe_open
        if {$::pipe_id ne ""} {
            after 200 [list pipe_send $cmd]
        }
    }
}

# ===================== Technology Configuration =====================
proc load_technology_from_state {} {
    set ::technology_rows {}
    set ::technology_names {}
    set path [gui_state_text exchange_technology_path]
    set content [load_text_file $path]
    foreach line [split $content "\n"] {
        if {$line eq "" || [string match "technology\t*" $line]} { continue }
        set fields [split $line "\t"]
        if {[llength $fields] < 12} { continue }
        set row [dict create \
            technology [lindex $fields 0] corner [lindex $fields 1] type [lindex $fields 2] \
            library [lindex $fields 3] voltage [lindex $fields 4] temperature [lindex $fields 5] \
            cells [lindex $fields 6] time_unit [lindex $fields 7] voltage_unit [lindex $fields 8] \
            leakage_unit [lindex $fields 9] capacitance_unit [lindex $fields 10] synthesis [lindex $fields 11]]
        lappend ::technology_rows $row
        set tech [dict get $row technology]
        if {[lsearch -exact $::technology_names $tech] < 0} { lappend ::technology_names $tech }
    }
    technology_refresh_list
}

proc technology_refresh_list {} {
    set listbox .toparea.right.pages.config.main.techs.list
    if {![winfo exists $listbox]} { return }
    $listbox delete 0 end
    set active_index -1
    set index 0
    foreach tech $::technology_names {
        set count 0
        foreach row $::technology_rows {
            if {[dict get $row technology] eq $tech} { incr count }
        }
        set marker [expr {$tech eq $::active_technology ? "*" : " "}]
        $listbox insert end [format "%s %-24s %2d" $marker $tech $count]
        if {$tech eq $::active_technology} { set active_index $index }
        incr index
    }
    if {$active_index >= 0} {
        $listbox selection clear 0 end
        $listbox selection set $active_index
        $listbox see $active_index
        set ::selected_technology [lindex $::technology_names $active_index]
    } elseif {[llength $::technology_names] > 0} {
        $listbox selection set 0
        set ::selected_technology [lindex $::technology_names 0]
    }
    technology_update_details
}

proc technology_select {} {
    set listbox .toparea.right.pages.config.main.techs.list
    set selection [$listbox curselection]
    if {[llength $selection] == 0} { return }
    set ::selected_technology [lindex $::technology_names [lindex $selection 0]]
    technology_update_details
}

proc technology_update_details {} {
    set widget .toparea.right.pages.config.main.details.text
    if {![winfo exists $widget]} { return }
    set rows {}
    set synthesis ""
    foreach row $::technology_rows {
        if {[dict get $row technology] eq $::selected_technology} {
            lappend rows $row
            if {[dict get $row synthesis]} { set synthesis [dict get $row corner] }
        }
    }
    $widget configure -state normal
    $widget delete 1.0 end
    if {[llength $rows] == 0} {
        $widget insert end "No Liberty libraries detected."
    } else {
        set first [lindex $rows 0]
        $widget insert end "Technology: $::selected_technology\n"
        $widget insert end "Corners: [llength $rows]\n"
        $widget insert end "Synthesis corner: $synthesis\n"
        $widget insert end "Units: time=[dict get $first time_unit], voltage=[dict get $first voltage_unit], leakage=[dict get $first leakage_unit], capacitance=[dict get $first capacitance_unit]\n\n"
        $widget insert end [format "%-32s %-4s %8s %8s %7s  %s\n" "Corner" "Type" "VDD" "Temp" "Cells" "Library"]
        $widget insert end [string repeat "-" 104]
        $widget insert end "\n"
        foreach row $rows {
            set synth_mark ""
            if {[dict get $row synthesis]} {
                set synth_mark { [SYNTH]}
            }
            $widget insert end [format "%-32s %-4s %7.3fV %7.1fC %7s  %s%s\n" \
                [dict get $row corner] [dict get $row type] [dict get $row voltage] \
                [dict get $row temperature] [dict get $row cells] [dict get $row library] $synth_mark]
        }
    }
    $widget configure -state disabled
    if {[winfo exists .toparea.right.pages.config.current.value]} {
        .toparea.right.pages.config.current.value configure -text \
            [expr {$::active_technology eq "" ? "No technology selected" : $::active_technology}]
    }
    if {[winfo exists .toparea.right.pages.config.actions.apply]} {
        set state [expr {$::selected_technology eq "" || $::selected_technology eq $::active_technology ? "disabled" : "normal"}]
        .toparea.right.pages.config.actions.apply configure -state $state
    }
}

proc technology_apply {} {
    if {$::selected_technology eq ""} { return }
    console_log "ai_digital ▸ /tech $::selected_technology" "cmd"
    pipe_send "/tech $::selected_technology"
}

# ===================== Corner Power Chart =====================
proc chart_palette {index} {
    set colors {#4fc3f7 #ffa726 #66bb6a #ef5350 #ab47bc #26a69a #d4e157 #ff7043 #7e57c2 #8d6e63}
    return [lindex $colors [expr {$index % [llength $colors]}]]
}

proc load_area_breakdown_from_state {} {
    set ::area_breakdown_rows {}
    set content [load_text_file [gui_state_text exchange_area_path]]
    set in_section 0
    foreach line [split $content "\n"] {
        set trimmed [string trim $line]
        if {$trimmed eq "Cell Breakdown"} {
            set in_section 1
            continue
        }
        if {!$in_section} { continue }
        if {$trimmed eq "" || [string match "Synthesis Report*" $trimmed]} { break }
        if {[string match "----*" $trimmed]} { continue }
        if {[regexp {^(\S+)\s+(\d+)\s+([0-9.]+)\s+GE$} $trimmed -> cell_type count total_ge]} {
            lappend ::area_breakdown_rows [dict create type $cell_type count $count total_ge $total_ge]
        }
    }
}

proc render_area_chart {} {
    set canvas .toparea.right.pages.area.main.chart.canvas
    if {![winfo exists $canvas]} { return }
    $canvas delete all
    if {[llength $::area_breakdown_rows] == 0} {
        canvas_message $canvas "No area breakdown" "Run synthesis to populate standard-cell area composition."
        return
    }
    set width [winfo width $canvas]
    set height [winfo height $canvas]
    if {$width < 420} { set width 620 }
    if {$height < 320} { set height 420 }
    set cx 160
    set cy [expr {$height / 2.0}]
    set radius [expr {min(130, ($height - 60) / 2.0)}]
    set total 0.0
    foreach row $::area_breakdown_rows { set total [expr {$total + [dict get $row total_ge]}] }
    if {$total <= 0} {
        canvas_message $canvas "No area breakdown" "Total area is zero."
        return
    }
    $canvas create text 16 18 -anchor w -text "Cell Area Composition" -fill $::C_HIGHLIGHT -font {Helvetica 10 bold}
    set start 0.0
    set legend_y 52
    set idx 0
    foreach row $::area_breakdown_rows {
        set value [dict get $row total_ge]
        set extent [expr {$value / $total * 360.0}]
        set color [chart_palette $idx]
        $canvas create arc [expr {$cx - $radius}] [expr {$cy - $radius}] [expr {$cx + $radius}] [expr {$cy + $radius}] \
            -start $start -extent $extent -fill $color -outline $::C_BG -width 1
        set pct [expr {$value * 100.0 / $total}]
        $canvas create rectangle 340 $legend_y 354 [expr {$legend_y + 14}] -fill $color -outline ""
        $canvas create text 362 [expr {$legend_y + 7}] -anchor w \
            -text [format "%s  %.1f%%  (%s cells / %.1f GE)" [dict get $row type] $pct [dict get $row count] $value] \
            -fill $::C_TEXT -font {Helvetica 8}
        set start [expr {$start + $extent}]
        incr legend_y 22
        incr idx
    }
    $canvas configure -scrollregion "0 0 $width [expr {max($height, $legend_y + 20)}]"
}

proc load_power_corners_from_state {} {
    set ::power_corner_rows {}
    set content [load_text_file [gui_state_text exchange_power_corners_path]]
    foreach line [split $content "\n"] {
        if {$line eq "" || [string match "analysis\t*" $line]} { continue }
        set fields [split $line "\t"]
        if {[llength $fields] < 8} { continue }
        lappend ::power_corner_rows [dict create analysis [lindex $fields 0] frequency [lindex $fields 1] \
            corner [lindex $fields 2] type [lindex $fields 3] voltage [lindex $fields 4] \
            static [lindex $fields 5] dynamic [lindex $fields 6] total [lindex $fields 7]]
    }
    render_power_chart
}

proc render_power_chart {} {
    set canvas .toparea.right.pages.power.main.chart.canvas
    if {![winfo exists $canvas]} { return }
    $canvas delete all
    if {[llength $::power_corner_rows] == 0} {
        canvas_message $canvas "No corner power data" "Run multi-corner power analysis to populate this chart."
        return
    }
    set width [winfo width $canvas]
    if {$width < 420} { set width 760 }
    array unset groups
    set order {}
    set max_total 0.0
    foreach row $::power_corner_rows {
        set key "[dict get $row analysis]|[format %.3f [dict get $row frequency]]"
        if {![info exists groups($key)]} {
            set groups($key) {}
            lappend order $key
        }
        lappend groups($key) $row
        if {[dict get $row total] > $max_total} { set max_total [dict get $row total] }
    }
    if {$max_total <= 0} { set max_total 1.0 }
    set row_height 42
    set height 60
    set label_width 190
    set right_pad 80
    $canvas create text 16 18 -anchor w -text "Per-Corner Power Across Operating Points" -fill $::C_HIGHLIGHT -font {Helvetica 10 bold}
    $canvas create rectangle 16 38 30 50 -fill $::C_WARN -outline ""
    $canvas create text 35 44 -anchor w -text "Static" -fill $::C_DIM -font {Helvetica 8}
    $canvas create rectangle 88 38 102 50 -fill $::C_HIGHLIGHT -outline ""
    $canvas create text 107 44 -anchor w -text "Non-static (dynamic/internal/clock)" -fill $::C_DIM -font {Helvetica 8}
    set y 72
    foreach key $order {
        set rows $groups($key)
        set first [lindex $rows 0]
        $canvas create text 12 $y -anchor w \
            -text "[dict get $first analysis] @ [format %.0f [dict get $first frequency]] MHz" \
            -fill $::C_HIGHLIGHT -font {Helvetica 9 bold}
        incr y 18
        foreach row $rows {
            set bar_x $label_width
            set usable [expr {$width - $label_width - $right_pad}]
            set static_w [expr {$usable * [dict get $row static] / $max_total}]
            set total_w [expr {$usable * [dict get $row total] / $max_total}]
            $canvas create text 12 [expr {$y + 11}] -anchor w \
                -text "[dict get $row corner] ([dict get $row type])" -fill $::C_TEXT -font {Courier 8}
            $canvas create rectangle $bar_x $y [expr {$bar_x + $total_w}] [expr {$y + 22}] -fill $::C_HIGHLIGHT -outline ""
            if {$static_w > 0} {
                $canvas create rectangle $bar_x $y [expr {$bar_x + $static_w}] [expr {$y + 22}] -fill $::C_WARN -outline ""
            }
            $canvas create text [expr {$bar_x + $total_w + 7}] [expr {$y + 11}] -anchor w \
                -text [format "%.2f uW" [dict get $row total]] -fill $::C_TEXT -font {Helvetica 8 bold}
            incr y $row_height
        }
        incr y 8
    }
    set height [expr {max($height, $y + 12)}]
    $canvas configure -scrollregion "0 0 $width $height"
}

# ===================== Update UI =====================
proc update_synth_stats {} {
    if {[winfo exists .toparea.right.pages.synth.stats.cells.v]} {
        .toparea.right.pages.synth.stats.cells.v configure -text $::synth_cells
        .toparea.right.pages.synth.stats.dff.v configure -text $::synth_dff
        .toparea.right.pages.synth.stats.area.v configure -text [format "%.1f" $::synth_area_ge]
        .toparea.right.pages.synth.stats.depth.v configure -text $::synth_depth
    }
}

proc update_timing_page {} {
    if {[winfo exists .toparea.right.pages.timing.toolbar.freq]} {
        .toparea.right.pages.timing.toolbar.freq delete 0 end
        .toparea.right.pages.timing.toolbar.freq insert 0 $::constraint_freq
    }
}

proc update_formal_page {} {
    if {[winfo exists .toparea.right.pages.formal.result]} {
        .toparea.right.pages.formal.result configure -text "  $::formal_result  "
        if {$::formal_result eq "PASS" || $::formal_result eq "EQUIVALENT"} {
            .toparea.right.pages.formal.result configure -fg $::C_OK -bg "#1b3d1b"
        } else {
            .toparea.right.pages.formal.result configure -fg $::C_ERR -bg "#3d1b1b"
        }
    }
    render_formal_points_graph
}

proc update_sim_page {} {
    if {[winfo exists .toparea.right.pages.sim.toolbar.status]} {
        if {[info exists ::gui_state(simulation_passed)] && $::gui_state(simulation_passed)} {
            .toparea.right.pages.sim.toolbar.status configure -text "PASS" -fg $::C_OK
        } else {
            .toparea.right.pages.sim.toolbar.status configure -text "READY" -fg $::C_DIM
        }
    }
}

proc update_power_page {} {
    if {[winfo exists .toparea.right.pages.power.stats.total.v]} {
        .toparea.right.pages.power.stats.total.v configure -text "[format %.4f $::power_total] mW"
        .toparea.right.pages.power.stats.static.v configure -text "[format %.4f $::power_static] mW"
        .toparea.right.pages.power.stats.dynamic.v configure -text "[format %.4f $::power_dynamic] mW"
    }
}

proc update_area_page {} {
    if {[winfo exists .toparea.right.pages.area.stats.ge.v]} {
        .toparea.right.pages.area.stats.ge.v configure -text [format "%.3f" $::synth_area_ge]
        .toparea.right.pages.area.stats.um2.v configure -text [format "%.3f" $::synth_area_um2]
        .toparea.right.pages.area.stats.cells.v configure -text $::synth_cells
    }
    load_area_breakdown_from_state
    render_area_chart
}

proc update_summary_page {} {
    if {[gui_state_get step_status] eq "failed"} {
        set lint_value "Check console"
    } else {
        set lint_value "Available"
    }
    if {$::timing_met} {
        set timing_status "✓"
        set timing_color $::C_OK
    } else {
        set timing_status "✗"
        set timing_color $::C_ERR
    }
    if {$::formal_result eq "PASS" || $::formal_result eq "EQUIVALENT"} {
        set formal_status "✓"
        set formal_color $::C_OK
    } else {
        set formal_status "○"
        set formal_color $::C_DIM
    }
    foreach {key value status color} [list \
        lint   $lint_value "○" $::C_DIM \
        synth  "$::synth_cells cells / [format %.0f $::synth_area_ge] GE" "○" $::C_DIM \
        area   "[format %.3f $::synth_area_ge] GE" "○" $::C_DIM \
        timing [format "%.3f ns" $::timing_slack] $timing_status $timing_color \
        power  "[format %.4f $::power_total] mW" "○" $::C_DIM \
        formal $::formal_result $formal_status $formal_color \
    ] {
        if {[winfo exists .toparea.right.pages.summary.body.$key.v]} {
            .toparea.right.pages.summary.body.$key.v configure -text $value
            .toparea.right.pages.summary.body.$key.s configure -text $status -fg $color
        }
    }
    render_hierarchy_treemap
}

# ===================== Page Switching =====================
proc set_page {page} {
    set ::current_page $page
    foreach p {config rtl sim synth timing power area formal summary} {
        if {[winfo exists .toparea.right.pages.$p]} { pack forget .toparea.right.pages.$p }
    }
    if {[winfo exists .toparea.right.pages.$page]} { pack .toparea.right.pages.$page -fill both -expand 1 }
    foreach p {config rtl sim synth timing power area formal summary} {
        if {[winfo exists .toparea.left.$p]} { .toparea.left.$p configure -bg $::C_PANEL -fg $::C_DIM }
    }
    if {[winfo exists .toparea.left.$page]} { .toparea.left.$page configure -bg $::C_ACCENT -fg $::C_HIGHLIGHT }
    if {$page eq "synth"} {
        after idle render_gate_netlist
    } elseif {$page eq "config"} {
        after idle technology_refresh_list
    } elseif {$page eq "power"} {
        after idle render_power_chart
    } elseif {$page eq "area"} {
        after idle render_area_chart
    } elseif {$page eq "timing"} {
        after idle render_timing_path_graph
    } elseif {$page eq "formal"} {
        after idle render_formal_points_graph
    } elseif {$page eq "summary"} {
        after idle render_hierarchy_treemap
    }
}

proc gui_write_canvas_postscript {canvas path} {
    if {![winfo exists $canvas]} { return }
    if {[catch {$canvas postscript -file $path -colormode color} err]} {
        debug_error "gui_write_canvas_postscript" "$canvas -> $path : $err"
    }
}

proc gui_canvas_summary {canvas} {
    if {![winfo exists $canvas]} { return "missing" }
    set bbox [$canvas bbox all]
    if {[llength $bbox] != 4} {
        return [format "items=%d bbox=empty" [llength [$canvas find all]]]
    }
    return [format "items=%d bbox=%s" [llength [$canvas find all]] $bbox]
}

proc gui_run_autodump {} {
    if {![info exists ::env(AI_DIGITAL_GUI_AUTODUMP)]} { return }
    set outdir $::env(AI_DIGITAL_GUI_AUTODUMP)
    catch {file mkdir $outdir}

    set summary_path [file join $outdir "canvas_summary.txt"]
    if {[catch {set fp [open $summary_path w]} err]} {
        debug_error "gui_run_autodump" "open summary failed: $err"
        after 200 exit
        return
    }

    foreach spec {
        {synth   .toparea.right.pages.synth.gate_canvas                 render_gate_netlist      synth_canvas}
        {timing  .toparea.right.pages.timing.vpane.graph.main.right.canvas render_timing_path_graph timing_canvas}
        {sim     .toparea.right.pages.sim.vpane.wave.main.view.canvas   render_waveform          waveform_canvas}
        {formal  .toparea.right.pages.formal.points_canvas              render_formal_points_graph formal_canvas}
        {power   .toparea.right.pages.power.main.chart.canvas           render_power_chart        power_canvas}
        {area    .toparea.right.pages.area.main.chart.canvas            render_area_chart         area_canvas}
        {summary .toparea.right.pages.summary.hpane.hier.canvas         render_hierarchy_treemap hierarchy_canvas}
    } {
        lassign $spec page canvas renderer stem
        catch {set_page $page}
        update idletasks
        catch {$renderer}
        update idletasks
        puts $fp [format "%-10s %s" $stem [gui_canvas_summary $canvas]]
        gui_write_canvas_postscript $canvas [file join $outdir "${stem}.ps"]
    }

    close $fp
    debug "GUI autodump written to $outdir"
    after 250 exit
}

# ===================== Project Actions =====================
proc load_rtl_file {} {
    set dir [tk_chooseDirectory -title "Open Project Folder" -mustexist 1]
    if {$dir eq ""} return
    set ::current_project_dir $dir
    console_log "Opening project: $dir" "ok"
    if {$::pipe_id ne ""} {
        pipe_send "/project open $dir"
    }
    if {[file exists [file join $dir "exchange" "gui_state.tcl"]]} {
        after 200 [list load_gui_state_file [file join $dir "exchange" "gui_state.tcl"]]
    }
}

# ===================== Build GUI =====================
debug "Building GUI..."
wm title . "$::APP_NAME v$::APP_VERSION"
wm geometry . 1280x800+50+50
wm minsize . 900 600
. configure -bg $::C_BG

# ===== Pack: bottom items first (status, console) =====
frame .status -bg $::C_PANEL -height 24 -relief sunken -bd 0
pack .status -fill x -side bottom
pack propagate .status 0

frame .console -bg $::C_INPUT_BG
pack .console -fill x -side bottom
frame .console.textframe -bg $::C_INPUT_BG
pack .console.textframe -fill both -expand 1 -side top
text .console.text -bg $::C_INPUT_BG -fg $::C_DIM -relief flat -bd 0 -font {Courier 10} -wrap word -state disabled -height 8
scrollbar .console.scroll -command {.console.text yview} -bg $::C_PANEL -troughcolor $::C_BG
.console.text configure -yscrollcommand {.console.scroll set}
pack .console.scroll -side right -fill y; pack .console.text -fill both -expand 1
.console.text tag configure ok_tag -foreground $::C_OK
.console.text tag configure err_tag -foreground $::C_ERR
.console.text tag configure warn_tag -foreground $::C_WARN
.console.text tag configure cmd_tag -foreground $::C_HIGHLIGHT
.console.text tag configure dim_tag -foreground $::C_DIM
.console.text tag configure bold_tag -font {Courier 10 bold}
.console.text tag configure green_tag -foreground $::C_OK
.console.text tag configure red_tag -foreground $::C_ERR
.console.text tag configure yellow_tag -foreground $::C_WARN
.console.text tag configure blue_tag -foreground $::C_HIGHLIGHT
.console.text tag configure cyan_tag -foreground "#4fc3f7"
.console.text tag configure magenta_tag -foreground "#ba68c8"
frame .console.inputframe -bg $::C_PANEL
pack .console.inputframe -fill x -side bottom
label .console.inputframe.prompt -text "  ai_digital ▸" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Courier 10 bold}
ttk::entry .console.input -style ConsoleInput.TEntry -font {Courier 10} \
    -exportselection 0 -takefocus 1
button .console.inputframe.send -text "Send" -command console_cmd \
    -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 10
bind .console.input <Return> {console_cmd; break}
bind .console.input <KP_Enter> {console_cmd; break}
bind .console.input <ButtonPress-1> {after idle {focus .console.input}}
bind .console.input <FocusIn> {
    catch {tk useinputmethods 1}
    catch {focus .console.input}
}
bind .console.input <Map> {
    catch {tk useinputmethods 1}
    focus_console_input
}
# Give focus to input after GUI loads
after 100 focus_console_input
pack .console.inputframe.prompt -side left
pack .console.input -side left -fill x -expand 1 -pady 3 -ipady 3
pack .console.inputframe.send -side right -padx 4 -pady 2

# ===== Title Bar =====
frame .topbar -bg $::C_TITLE_BG -height 36 -bd 0
pack .topbar -fill x -side top
pack propagate .topbar 0
label .topbar.logo -text "  $::APP_NAME  " -fg $::C_HIGHLIGHT -bg $::C_TITLE_BG -font {Helvetica 13 bold}
label .topbar.ver -text "v$::APP_VERSION" -fg $::C_DIM -bg $::C_TITLE_BG -font {Helvetica 9}
pack .topbar.logo -side left; pack .topbar.ver -side left

# ===== Menu Bar =====
menu .menubar -bg $::C_PANEL -fg $::C_TEXT -activebackground $::C_ACCENT -activeforeground $::C_HIGHLIGHT
. configure -menu .menubar
menu .menubar.file -tearoff 0 -bg $::C_PANEL -fg $::C_TEXT -activebackground $::C_ACCENT -activeforeground $::C_HIGHLIGHT
.menubar add cascade -label "File" -menu .menubar.file -underline 0
.menubar.file add command -label "Open Project...   Ctrl+O" -command load_rtl_file
.menubar.file add command -label "Quit              Ctrl+Q" -command exit
menu .menubar.flow -tearoff 0 -bg $::C_PANEL -fg $::C_TEXT -activebackground $::C_ACCENT -activeforeground $::C_HIGHLIGHT
.menubar add cascade -label "Flow" -menu .menubar.flow -underline 0
.menubar.flow add command -label "Full Flow" -command {console_log "ai_digital ▸ /full" "cmd"; pipe_send "/full"}
.menubar.flow add command -label "Lint" -command {console_log "ai_digital ▸ /lint $::current_module" "cmd"; pipe_send "/lint $::current_module"}
.menubar.flow add command -label "Synthesis" -command {console_log "ai_digital ▸ /synth $::current_module" "cmd"; pipe_send "/synth $::current_module"}
menu .menubar.help -tearoff 0 -bg $::C_PANEL -fg $::C_TEXT -activebackground $::C_ACCENT -activeforeground $::C_HIGHLIGHT
.menubar add cascade -label "Help" -menu .menubar.help -underline 0
.menubar.help add command -label "About" -command {tk_messageBox -title "About" -message "AI Digital v$::APP_VERSION\nRTL Design & Analysis Suite\n\nPipe-integrated CLI mode"}

# ===== Top Area =====
frame .toparea -bg $::C_BG
pack .toparea -fill both -expand 1 -side top

# ===== LEFT SIDEBAR =====
frame .toparea.left -width 180 -bg $::C_PANEL -bd 0 -highlightthickness 0
pack .toparea.left -side left -fill y -anchor nw
label .toparea.left.title -text "  Flow Steps" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 11 bold}
pack .toparea.left.title -fill x -pady {10 6}
set ::steps {config "⚙ Config" rtl "📄 RTL" sim "▶ Simulation" synth "⚙ Synthesis" timing "⏱ Timing" power "⚡ Power" area "📐 Area" formal "✓ Formal" summary "📊 Summary"}
foreach {key label} $::steps {
    button .toparea.left.$key -text $label -bg $::C_PANEL -fg $::C_DIM \
        -activebackground "#33334a" -activeforeground $::C_HIGHLIGHT \
        -relief flat -borderwidth 0 -anchor w -padx 14 -pady 2 \
        -font {Helvetica 10} -command [list set_page $key]
    pack .toparea.left.$key -fill x -ipady 5
}
frame .toparea.left.sep1 -bg $::C_BORDER -height 1; pack .toparea.left.sep1 -fill x -pady {12 6}
label .toparea.left.proj_title -text "  Project" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 11 bold}
pack .toparea.left.proj_title -fill x
label .toparea.left.project -text "  $::project_name" -fg $::C_TEXT -bg $::C_PANEL -anchor w -font {Helvetica 10}
pack .toparea.left.project -fill x -pady {2 0}
label .toparea.left.mod_title -text "  Module" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 11 bold}
pack .toparea.left.mod_title -fill x -pady {6 0}
label .toparea.left.module -text "  (none)" -fg $::C_DIM -bg $::C_PANEL -anchor w -font {Helvetica 10}
pack .toparea.left.module -fill x -pady {2 0}

# ===== RIGHT CONTENT =====
frame .toparea.right -bg $::C_BG
pack .toparea.right -side left -fill both -expand 1
set ::page_container [frame .toparea.right.pages -bg $::C_BG]
pack $::page_container -fill both -expand 1

# --- Page: Config ---
set p [frame .toparea.right.pages.config -bg $::C_BG]
frame $p.header -bg $::C_PANEL
pack $p.header -fill x
label $p.header.title -text "  Project Technology" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 11 bold}
pack $p.header.title -side left -padx 4 -pady 7
panedwindow $p.main -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.main -fill both -expand 1 -padx 6 -pady 6
set tech_list_frame [frame $p.main.techs -bg $::C_PANEL -width 300]
label $tech_list_frame.title -text "  Available Technologies" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $tech_list_frame.title -fill x -pady {5 3}
listbox $tech_list_frame.list -selectmode browse -bg $::C_INPUT_BG -fg $::C_TEXT -font {Courier 9} \
    -highlightbackground $::C_BORDER -exportselection 0 -activestyle dotbox
scrollbar $tech_list_frame.scroll -command [list $tech_list_frame.list yview] -bg $::C_PANEL -troughcolor $::C_BG
$tech_list_frame.list configure -yscrollcommand [list $tech_list_frame.scroll set]
pack $tech_list_frame.scroll -side right -fill y -padx {0 3} -pady 3
pack $tech_list_frame.list -side left -fill both -expand 1 -padx {4 0} -pady 3
bind $tech_list_frame.list <<ListboxSelect>> technology_select
set tech_detail_frame [frame $p.main.details -bg $::C_INPUT_BG]
label $tech_detail_frame.title -text "  Liberty Information" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $tech_detail_frame.title -fill x -pady {5 3}
text $tech_detail_frame.text -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 0 -font {Courier 9} -state disabled -wrap none
scrollbar $tech_detail_frame.ys -command [list $tech_detail_frame.text yview] -bg $::C_PANEL -troughcolor $::C_BG
scrollbar $tech_detail_frame.xs -orient horizontal -command [list $tech_detail_frame.text xview] -bg $::C_PANEL -troughcolor $::C_BG
$tech_detail_frame.text configure -yscrollcommand [list $tech_detail_frame.ys set] -xscrollcommand [list $tech_detail_frame.xs set]
pack $tech_detail_frame.ys -side right -fill y
pack $tech_detail_frame.xs -side bottom -fill x
pack $tech_detail_frame.text -fill both -expand 1 -padx 4 -pady 3
$p.main add $tech_list_frame -width 300 -sticky news
$p.main add $tech_detail_frame -width 680 -sticky news
frame $p.current -bg $::C_PANEL
pack $p.current -fill x -padx 6 -pady {0 2}
label $p.current.label -text "  Current project technology:" -fg $::C_DIM -bg $::C_PANEL -font {Helvetica 10}
label $p.current.value -text "No technology selected" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 10 bold}
pack $p.current.label -side left -pady 7
pack $p.current.value -side left -padx 8 -pady 7
frame $p.actions -bg $::C_BG
pack $p.actions -fill x -padx 6 -pady {2 7}
button $p.actions.apply -text "Use for Project" -command technology_apply -bg $::C_ACCENT -fg $::C_HIGHLIGHT \
    -activebackground "#33334a" -activeforeground $::C_HIGHLIGHT -relief flat -padx 12 -pady 4 -state disabled
pack $p.actions.apply -side right
pack forget $p

# --- Page: RTL ---
set p [frame .toparea.right.pages.rtl -bg $::C_BG]
frame $p.toolbar -bg $::C_PANEL -height 32; pack $p.toolbar -fill x
button $p.toolbar.open -text "Open Project" -command load_rtl_file -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
button $p.toolbar.lint -text "Lint" -command {console_log "ai_digital ▸ /lint $::current_module" "cmd"; pipe_send "/lint $::current_module"} -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
pack $p.toolbar.open -side left -padx 4 -pady 2; pack $p.toolbar.lint -side left -padx 4 -pady 2
panedwindow $p.hpane -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.hpane -fill both -expand 1 -padx 6 -pady 4
set rtl_frame [frame $p.hpane.rtl -bg $::C_INPUT_BG]
frame $rtl_frame.bar -bg $::C_PANEL
pack $rtl_frame.bar -fill x
label $rtl_frame.bar.title -text "  RTL Source" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
pack $rtl_frame.bar.title -side left -padx 4 -pady 2
text $p.code -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled
pack $p.code -fill both -expand 1 -padx 2 -pady 2 -in $rtl_frame
set tb_frame [frame $p.hpane.tb -bg $::C_INPUT_BG]
frame $tb_frame.bar -bg $::C_PANEL
pack $tb_frame.bar -fill x
label $tb_frame.bar.title -text "  Testbench" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
pack $tb_frame.bar.title -side left -padx 4 -pady 2
text $p.tb -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled
pack $p.tb -fill both -expand 1 -padx 2 -pady 2 -in $tb_frame
$p.hpane add $rtl_frame -width 560 -sticky news
$p.hpane add $tb_frame -width 420 -sticky news
pack forget $p

# --- Page: Simulation ---
set p [frame .toparea.right.pages.sim -bg $::C_BG]
frame $p.toolbar -bg $::C_PANEL; pack $p.toolbar -fill x
button $p.toolbar.run -text "Run Simulation" -command {console_log "ai_digital ▸ /sim $::current_module" "cmd"; pipe_send "/sim $::current_module"} -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
pack $p.toolbar.run -side left -padx 4 -pady 2
label $p.toolbar.status -text "" -fg $::C_DIM -bg $::C_PANEL; pack $p.toolbar.status -side left -padx 8
panedwindow $p.vpane -orient vertical -bg $::C_BORDER -sashwidth 4
pack $p.vpane -fill both -expand 1 -padx 6 -pady 4
set wave_frame [frame $p.vpane.wave -bg $::C_INPUT_BG]
frame $wave_frame.bar -bg $::C_PANEL
pack $wave_frame.bar -fill x
label $wave_frame.bar.title -text "  Waveform Viewer" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
button $wave_frame.bar.zoom_in -text "+" -command wave_zoom_in -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
button $wave_frame.bar.zoom_out -text "-" -command wave_zoom_out -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
button $wave_frame.bar.zoom_reset -text "1:1" -command wave_zoom_reset -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 3
pack $wave_frame.bar.title -side left -padx 4
pack $wave_frame.bar.zoom_reset -side right -padx 2
pack $wave_frame.bar.zoom_out -side right -padx 2
pack $wave_frame.bar.zoom_in -side right -padx 2
frame $wave_frame.main -bg $::C_INPUT_BG
pack $wave_frame.main -fill both -expand 1
frame $wave_frame.main.ctrl -bg $::C_PANEL -width 230
pack $wave_frame.main.ctrl -side left -fill y
label $wave_frame.main.ctrl.title -text "  Visible Signals" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $wave_frame.main.ctrl.title -fill x -pady {4 2}
listbox $wave_frame.main.ctrl.list -selectmode extended -bg $::C_INPUT_BG -fg $::C_TEXT -font {Courier 9} -highlightbackground $::C_BORDER -exportselection 0
scrollbar $wave_frame.main.ctrl.scroll -command [list $wave_frame.main.ctrl.list yview] -bg $::C_PANEL -troughcolor $::C_BG
$wave_frame.main.ctrl.list configure -yscrollcommand [list $wave_frame.main.ctrl.scroll set]
pack $wave_frame.main.ctrl.scroll -side right -fill y -padx {0 2} -pady 2
pack $wave_frame.main.ctrl.list -side left -fill both -expand 1 -padx {4 0} -pady 2
frame $wave_frame.main.ctrl.buttons -bg $::C_PANEL
pack $wave_frame.main.ctrl.buttons -fill x -pady 4
button $wave_frame.main.ctrl.buttons.apply -text "Show Selected" -command wave_apply_signal_selection -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat
button $wave_frame.main.ctrl.buttons.all -text "Show All" -command wave_show_all -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat
button $wave_frame.main.ctrl.buttons.clear -text "Clear" -command wave_clear_selection -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat
pack $wave_frame.main.ctrl.buttons.apply -fill x -padx 4 -pady 1
pack $wave_frame.main.ctrl.buttons.all -fill x -padx 4 -pady 1
pack $wave_frame.main.ctrl.buttons.clear -fill x -padx 4 -pady 1
bind $wave_frame.main.ctrl.list <<ListboxSelect>> wave_apply_signal_selection
frame $wave_frame.main.view -bg $::C_INPUT_BG
pack $wave_frame.main.view -side left -fill both -expand 1
canvas $wave_frame.main.view.canvas -bg $::C_INPUT_BG -highlightthickness 0
scrollbar $wave_frame.main.view.vs -orient vertical -command [list $wave_frame.main.view.canvas yview] -bg $::C_PANEL -troughcolor $::C_BG
scrollbar $wave_frame.main.view.hs -orient horizontal -command [list $wave_frame.main.view.canvas xview] -bg $::C_PANEL -troughcolor $::C_BG
$wave_frame.main.view.canvas configure -yscrollcommand [list $wave_frame.main.view.vs set] -xscrollcommand [list $wave_frame.main.view.hs set]
bind $wave_frame.main.view.canvas <Button-4> {wave_zoom_in; break}
bind $wave_frame.main.view.canvas <Button-5> {wave_zoom_out; break}
bind $wave_frame.main.view.canvas <MouseWheel> {
    if {%D > 0} {wave_zoom_in} else {wave_zoom_out}
    break
}
bind $wave_frame.main.view.canvas <ButtonPress-1> {canvas_pan_mark %W %x %y}
bind $wave_frame.main.view.canvas <B1-Motion> {canvas_pan_drag %W %x %y}
pack $wave_frame.main.view.vs -side right -fill y
pack $wave_frame.main.view.hs -side bottom -fill x
pack $wave_frame.main.view.canvas -side left -fill both -expand 1
set log_frame [frame $p.vpane.log -bg $::C_INPUT_BG]
frame $log_frame.bar -bg $::C_PANEL
pack $log_frame.bar -fill x
label $log_frame.bar.title -text "  Simulation Log" -fg $::C_DIM -bg $::C_PANEL -font {Helvetica 9 bold}
pack $log_frame.bar.title -side left -padx 4 -pady 2
text $p.out -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled
pack $p.out -fill both -expand 1 -in $log_frame -padx 2 -pady 2
$p.vpane add $wave_frame -height 340 -sticky news
$p.vpane add $log_frame -height 180 -sticky news
pack forget $p

# --- Page: Synthesis (horizontal layout: diagram | text) ---
set p [frame .toparea.right.pages.synth -bg $::C_BG]
frame $p.toolbar -bg $::C_PANEL; pack $p.toolbar -fill x
button $p.toolbar.run -text "Run Synthesis" -command {console_log "ai_digital ▸ /synth $::current_module" "cmd"; pipe_send "/synth $::current_module"} -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
pack $p.toolbar.run -side left -padx 4 -pady 2
label $p.toolbar.status -text "(not run)" -fg $::C_DIM -bg $::C_PANEL; pack $p.toolbar.status -side left -padx 8

# Stats bar
frame $p.stats -bg $::C_INPUT_BG -bd 1 -highlightbackground $::C_BORDER -highlightthickness 1
pack $p.stats -fill x -padx 6 -pady 4
foreach {key label} {cells "Cells:" dff "DFFs:" area "Area (GE):" depth "Logic Depth:"} {
    frame $p.stats.$key -bg $::C_INPUT_BG; pack $p.stats.$key -fill x -ipady 2
    label $p.stats.$key.l -text "  $label" -fg $::C_DIM -bg $::C_INPUT_BG -width 15 -anchor w -font {Helvetica 10}
    label $p.stats.$key.v -text "0" -fg $::C_HIGHLIGHT -bg $::C_INPUT_BG -anchor e -font {Helvetica 10 bold}
    pack $p.stats.$key.l -side left; pack $p.stats.$key.v -side right -padx 8
}

# Horizontal split: circuit diagram (left, 70%) | text netlist (right, 30%)
panedwindow $p.hpane -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.hpane -fill both -expand 1 -padx 6 -pady 2

# Left pane: circuit diagram canvas with scrollbars and zoom
set left_frame [frame $p.hpane.left -bg $::C_INPUT_BG]
# Zoom toolbar
frame $left_frame.zoom -bg $::C_PANEL
pack $left_frame.zoom -fill x -side top
label $left_frame.zoom.title -text "  Circuit Diagram" -fg $::C_DIM -bg $::C_PANEL -font {Helvetica 9 bold}
button $left_frame.zoom.in -text "+" -command gate_zoom_in -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
button $left_frame.zoom.out -text "-" -command gate_zoom_out -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
button $left_frame.zoom.fit -text "1:1" -command gate_zoom_reset -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
pack $left_frame.zoom.title -side left -padx 4
pack $left_frame.zoom.fit -side right -padx 2; pack $left_frame.zoom.out -side right -padx 2; pack $left_frame.zoom.in -side right -padx 2

# Canvas with scrollbars
frame $left_frame.canvas_frame -bg $::C_INPUT_BG
pack $left_frame.canvas_frame -fill both -expand 1
canvas $p.gate_canvas -bg $::C_INPUT_BG -highlightthickness 0 -width 400 -height 300
scrollbar $p.gate_canvas_v -orient vertical -command [list $p.gate_canvas yview] -bg $::C_PANEL -troughcolor $::C_BG
scrollbar $p.gate_canvas_h -orient horizontal -command [list $p.gate_canvas xview] -bg $::C_PANEL -troughcolor $::C_BG
$p.gate_canvas configure -yscrollcommand [list $p.gate_canvas_v set] -xscrollcommand [list $p.gate_canvas_h set]
# Mouse wheel zoom
bind $p.gate_canvas <Button-4> {gate_zoom_in; break}
bind $p.gate_canvas <Button-5> {gate_zoom_out; break}
bind $p.gate_canvas <MouseWheel> {
    if {%D > 0} {gate_zoom_in} else {gate_zoom_out}
    break
}
bind $p.gate_canvas <ButtonPress-1> {canvas_pan_mark %W %x %y}
bind $p.gate_canvas <B1-Motion> {canvas_pan_drag %W %x %y}
bind $p.gate_canvas <Configure> {after idle render_gate_netlist}
pack $p.gate_canvas_v -side right -fill y -in $left_frame.canvas_frame
pack $p.gate_canvas_h -side bottom -fill x -in $left_frame.canvas_frame
pack $p.gate_canvas -fill both -expand 1 -in $left_frame.canvas_frame

# Right pane: text netlist
set right_frame [frame $p.hpane.right -bg $::C_INPUT_BG]
label $right_frame.title -text "  Gate Netlist" -fg $::C_DIM -bg $::C_PANEL -font {Helvetica 9 bold} -anchor w
pack $right_frame.title -fill x -padx 2 -pady 1
text $p.gate_text -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 0 -font {Courier 8} -width 30
scrollbar $right_frame.scroll -command [list $p.gate_text yview] -bg $::C_PANEL -troughcolor $::C_BG
$p.gate_text configure -yscrollcommand [list $right_frame.scroll set]
pack $right_frame.scroll -side right -fill y
pack $p.gate_text -fill both -expand 1 -padx 2 -pady 2 -in $right_frame

$p.hpane add $left_frame -width 500 -sticky news
$p.hpane add $right_frame -width 200 -sticky news
pack forget $p

# --- Page: Timing ---
set p [frame .toparea.right.pages.timing -bg $::C_BG]
frame $p.toolbar -bg $::C_PANEL; pack $p.toolbar -fill x
label $p.toolbar.fl -text "Freq (MHz):" -fg $::C_TEXT -bg $::C_PANEL -font {Helvetica 10}
entry $p.toolbar.freq -width 8 -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -highlightbackground $::C_BORDER
$p.toolbar.freq insert 0 "100"
button $p.toolbar.run -text "Analyze" -command {
    set ::constraint_freq [.toparea.right.pages.timing.toolbar.freq get]
    console_log "ai_digital ▸ /set freq $::constraint_freq" "cmd"
    pipe_send "/set freq $::constraint_freq"
    after 120 {
        console_log "ai_digital ▸ /timing $::current_module" "cmd"
        pipe_send "/timing $::current_module"
    }
} -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
pack $p.toolbar.fl -side left -padx 4; pack $p.toolbar.freq -side left; pack $p.toolbar.run -side left -padx 8 -pady 2
panedwindow $p.vpane -orient vertical -bg $::C_BORDER -sashwidth 4
pack $p.vpane -fill both -expand 1 -padx 6 -pady 4
set graph_frame [frame $p.vpane.graph -bg $::C_INPUT_BG]
frame $graph_frame.bar -bg $::C_PANEL
pack $graph_frame.bar -fill x
label $graph_frame.bar.title -text "  Critical Path Viewer" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
button $graph_frame.bar.zoom_in -text "+" -command timing_zoom_in -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
button $graph_frame.bar.zoom_out -text "-" -command timing_zoom_out -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 2
button $graph_frame.bar.zoom_reset -text "1:1" -command timing_zoom_reset -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -width 3
pack $graph_frame.bar.title -side left -padx 4
pack $graph_frame.bar.zoom_reset -side right -padx 2
pack $graph_frame.bar.zoom_out -side right -padx 2
pack $graph_frame.bar.zoom_in -side right -padx 2
panedwindow $graph_frame.main -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $graph_frame.main -fill both -expand 1
set path_frame [frame $graph_frame.main.left -bg $::C_PANEL -width 290]
label $path_frame.title -text "  Top 10 Critical Paths" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $path_frame.title -fill x -pady {4 2}
listbox $path_frame.list -selectmode browse -bg $::C_INPUT_BG -fg $::C_TEXT -font {Courier 8} -highlightbackground $::C_BORDER -exportselection 0
scrollbar $path_frame.scroll -command [list $path_frame.list yview] -bg $::C_PANEL -troughcolor $::C_BG
$path_frame.list configure -yscrollcommand [list $path_frame.scroll set]
pack $path_frame.scroll -side right -fill y -padx {0 2} -pady 2
pack $path_frame.list -side left -fill both -expand 1 -padx {4 0} -pady 2
bind $path_frame.list <<ListboxSelect>> timing_select_path
set path_canvas_frame [frame $graph_frame.main.right -bg $::C_INPUT_BG]
canvas $path_canvas_frame.canvas -bg $::C_INPUT_BG -highlightthickness 0
scrollbar $path_canvas_frame.vs -orient vertical -command [list $path_canvas_frame.canvas yview] -bg $::C_PANEL -troughcolor $::C_BG
scrollbar $path_canvas_frame.hs -orient horizontal -command [list $path_canvas_frame.canvas xview] -bg $::C_PANEL -troughcolor $::C_BG
$path_canvas_frame.canvas configure -yscrollcommand [list $path_canvas_frame.vs set] -xscrollcommand [list $path_canvas_frame.hs set]
bind $path_canvas_frame.canvas <Button-4> {timing_zoom_in; break}
bind $path_canvas_frame.canvas <Button-5> {timing_zoom_out; break}
bind $path_canvas_frame.canvas <MouseWheel> {
    if {%D > 0} {timing_zoom_in} else {timing_zoom_out}
    break
}
bind $path_canvas_frame.canvas <ButtonPress-1> {canvas_pan_mark %W %x %y}
bind $path_canvas_frame.canvas <B1-Motion> {canvas_pan_drag %W %x %y}
bind $path_canvas_frame.canvas <Configure> {after idle render_timing_path_graph}
pack $path_canvas_frame.vs -side right -fill y -in $path_canvas_frame
pack $path_canvas_frame.hs -side bottom -fill x -in $path_canvas_frame
pack $path_canvas_frame.canvas -fill both -expand 1 -in $path_canvas_frame
$graph_frame.main add $path_frame -width 290 -sticky news
$graph_frame.main add $path_canvas_frame -width 640 -sticky news
set text_frame [frame $p.vpane.text -bg $::C_INPUT_BG]
frame $text_frame.bar -bg $::C_PANEL
pack $text_frame.bar -fill x
label $text_frame.bar.title -text "  Timing Report" -fg $::C_DIM -bg $::C_PANEL -font {Helvetica 9 bold}
pack $text_frame.bar.title -side left -padx 4 -pady 2
text $p.out -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled
pack $p.out -fill both -expand 1 -in $text_frame -padx 2 -pady 2
$p.vpane add $graph_frame -height 360 -sticky news
$p.vpane add $text_frame -height 190 -sticky news
pack forget $p

# --- Page: Power ---
set p [frame .toparea.right.pages.power -bg $::C_BG]
frame $p.toolbar -bg $::C_PANEL; pack $p.toolbar -fill x
button $p.toolbar.run -text "Analyze Power" -command {console_log "ai_digital ▸ /power $::current_module" "cmd"; pipe_send "/power $::current_module"} -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
pack $p.toolbar.run -side left -padx 4 -pady 2
frame $p.stats -bg $::C_INPUT_BG -bd 1 -highlightbackground $::C_BORDER -highlightthickness 1
pack $p.stats -fill x -padx 6 -pady 4
foreach {key label} {total "Total Power:" static "Static:" dynamic "Dynamic:"} {
    frame $p.stats.$key -bg $::C_INPUT_BG; pack $p.stats.$key -fill x -ipady 2
    label $p.stats.$key.l -text "  $label" -fg $::C_DIM -bg $::C_INPUT_BG -width 15 -anchor w -font {Helvetica 10}
    label $p.stats.$key.v -text "0.0 mW" -fg $::C_HIGHLIGHT -bg $::C_INPUT_BG -anchor e -font {Helvetica 10 bold}
    pack $p.stats.$key.l -side left; pack $p.stats.$key.v -side right -padx 8
}
panedwindow $p.main -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.main -fill both -expand 1 -padx 6 -pady 4
set power_chart_frame [frame $p.main.chart -bg $::C_INPUT_BG]
label $power_chart_frame.title -text "  Corner Power Distribution" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $power_chart_frame.title -fill x -pady {3 2}
canvas $power_chart_frame.canvas -bg $::C_INPUT_BG -highlightthickness 0
scrollbar $power_chart_frame.scroll -command [list $power_chart_frame.canvas yview] -bg $::C_PANEL -troughcolor $::C_BG
$power_chart_frame.canvas configure -yscrollcommand [list $power_chart_frame.scroll set]
bind $power_chart_frame.canvas <Configure> {after idle render_power_chart}
pack $power_chart_frame.scroll -side right -fill y
pack $power_chart_frame.canvas -fill both -expand 1
set power_report_frame [frame $p.main.report -bg $::C_INPUT_BG]
label $power_report_frame.title -text "  Power Report" -fg $::C_DIM -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $power_report_frame.title -fill x -pady {3 2}
text $power_report_frame.out -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 9} -highlightbackground $::C_BORDER -state disabled -wrap none
scrollbar $power_report_frame.scroll -command [list $power_report_frame.out yview] -bg $::C_PANEL -troughcolor $::C_BG
$power_report_frame.out configure -yscrollcommand [list $power_report_frame.scroll set]
pack $power_report_frame.scroll -side right -fill y
pack $power_report_frame.out -fill both -expand 1 -padx 2 -pady 2
$p.main add $power_chart_frame -width 620 -sticky news
$p.main add $power_report_frame -width 420 -sticky news
pack forget $p

# --- Page: Area ---
set p [frame .toparea.right.pages.area -bg $::C_BG]
frame $p.stats -bg $::C_INPUT_BG -bd 1 -highlightbackground $::C_BORDER -highlightthickness 1
pack $p.stats -fill x -padx 6 -pady 4
foreach {key label} {ge "Area (GE):" um2 "Area (um^2):" cells "Cell Count:"} {
    frame $p.stats.$key -bg $::C_INPUT_BG; pack $p.stats.$key -fill x -ipady 2
    label $p.stats.$key.l -text "  $label" -fg $::C_DIM -bg $::C_INPUT_BG -width 15 -anchor w -font {Helvetica 10}
    label $p.stats.$key.v -text "0" -fg $::C_HIGHLIGHT -bg $::C_INPUT_BG -anchor e -font {Helvetica 10 bold}
    pack $p.stats.$key.l -side left; pack $p.stats.$key.v -side right -padx 8
}
panedwindow $p.main -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.main -fill both -expand 1 -padx 6 -pady 4
set area_chart_frame [frame $p.main.chart -bg $::C_INPUT_BG]
label $area_chart_frame.title -text "  Standard-Cell Area Share" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $area_chart_frame.title -fill x -pady {3 2}
canvas $area_chart_frame.canvas -bg $::C_INPUT_BG -highlightthickness 0
bind $area_chart_frame.canvas <Configure> {after idle render_area_chart}
pack $area_chart_frame.canvas -fill both -expand 1
set area_report_frame [frame $p.main.report -bg $::C_INPUT_BG]
label $area_report_frame.title -text "  Area Report" -fg $::C_DIM -bg $::C_PANEL -anchor w -font {Helvetica 9 bold}
pack $area_report_frame.title -fill x -pady {3 2}
text $p.out -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled
pack $p.out -fill both -expand 1 -in $area_report_frame -padx 2 -pady 2
$p.main add $area_chart_frame -width 520 -sticky news
$p.main add $area_report_frame -width 520 -sticky news
pack forget $p

# --- Page: Formal ---
set p [frame .toparea.right.pages.formal -bg $::C_BG]
frame $p.toolbar -bg $::C_PANEL; pack $p.toolbar -fill x
button $p.toolbar.run -text "Run Verification" -command {console_log "ai_digital ▸ /formal $::current_module" "cmd"; pipe_send "/formal $::current_module"} -bg $::C_ACCENT -fg $::C_HIGHLIGHT -relief flat -padx 8
pack $p.toolbar.run -side left -padx 4 -pady 2
label $p.toolbar.rl -text "Result: " -fg $::C_DIM -bg $::C_PANEL; pack $p.toolbar.rl -side left -padx 8
label $p.result -text "  (not run)  " -fg $::C_DIM -bg $::C_INPUT_BG -font {Helvetica 12 bold} -bd 1 -highlightbackground $::C_BORDER -highlightthickness 1
pack $p.result -side left
panedwindow $p.hpane -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.hpane -fill both -expand 1 -padx 6 -pady 4
set formal_report_frame [frame $p.hpane.report -bg $::C_INPUT_BG]
frame $formal_report_frame.bar -bg $::C_PANEL
pack $formal_report_frame.bar -fill x
label $formal_report_frame.bar.title -text "  Formal Report" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
pack $formal_report_frame.bar.title -side left -padx 4 -pady 2
text $p.out -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled
pack $p.out -fill both -expand 1 -in $formal_report_frame -padx 2 -pady 2
set formal_points_frame [frame $p.hpane.points -bg $::C_INPUT_BG]
frame $formal_points_frame.bar -bg $::C_PANEL
pack $formal_points_frame.bar -fill x
label $formal_points_frame.bar.title -text "  Verified Equivalence Points" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
pack $formal_points_frame.bar.title -side left -padx 4 -pady 2
canvas $p.points_canvas -bg $::C_INPUT_BG -highlightthickness 0
bind $p.points_canvas <Configure> {after idle render_formal_points_graph}
pack $p.points_canvas -fill both -expand 1 -in $formal_points_frame -padx 2 -pady 2
$p.hpane add $formal_report_frame -width 520 -sticky news
$p.hpane add $formal_points_frame -width 420 -sticky news
pack forget $p

# --- Page: Summary ---
set p [frame .toparea.right.pages.summary -bg $::C_BG]
frame $p.header -bg $::C_ACCENT -bd 0; pack $p.header -fill x -padx 6 -pady {8 0}
label $p.header.m -text "  Metric" -fg $::C_HIGHLIGHT -bg $::C_ACCENT -width 20 -anchor w -font {Helvetica 10 bold}
label $p.header.v -text "Value" -fg $::C_HIGHLIGHT -bg $::C_ACCENT -width 25 -anchor w -font {Helvetica 10 bold}
label $p.header.s -text "Status" -fg $::C_HIGHLIGHT -bg $::C_ACCENT -width 8 -anchor w -font {Helvetica 10 bold}
pack $p.header.m -side left -padx 4 -pady 2; pack $p.header.v -side left; pack $p.header.s -side left
panedwindow $p.hpane -orient horizontal -bg $::C_BORDER -sashwidth 4
pack $p.hpane -fill both -expand 1 -padx 6 -pady 0
set metrics_frame [frame $p.hpane.metrics -bg $::C_INPUT_BG]
frame $p.body -bg $::C_INPUT_BG -bd 1 -highlightbackground $::C_BORDER -highlightthickness 1
pack $p.body -fill both -expand 1 -in $metrics_frame
foreach {key label} {lint "Lint Check" synth "Synthesis" area "Area" timing "Timing" power "Power" formal "Formal Verification"} {
    set row [frame $p.body.$key -bg $::C_INPUT_BG]; pack $row -fill x -ipady 4
    label $row.m -text "  $label" -fg $::C_TEXT -bg $::C_INPUT_BG -width 20 -anchor w -font {Helvetica 10}
    label $row.v -text "-" -fg $::C_DIM -bg $::C_INPUT_BG -width 25 -anchor w -font {Helvetica 10}
    label $row.s -text "○" -fg $::C_DIM -bg $::C_INPUT_BG -width 8 -anchor w -font {Helvetica 10}
    pack $row.m -side left -padx 4; pack $row.v -side left; pack $row.s -side left
}
set hier_frame [frame $p.hpane.hier -bg $::C_INPUT_BG]
frame $hier_frame.bar -bg $::C_PANEL
pack $hier_frame.bar -fill x
label $hier_frame.bar.title -text "  Module Hierarchy Treemap" -fg $::C_HIGHLIGHT -bg $::C_PANEL -font {Helvetica 9 bold}
pack $hier_frame.bar.title -side left -padx 4 -pady 2
canvas $hier_frame.canvas -bg $::C_INPUT_BG -highlightthickness 0
pack $hier_frame.canvas -fill both -expand 1 -padx 2 -pady 2
$p.hpane add $metrics_frame -width 470 -sticky news
$p.hpane add $hier_frame -width 420 -sticky news
text $p.out -bg $::C_INPUT_BG -fg $::C_TEXT -relief flat -bd 1 -font {Courier 10} -highlightbackground $::C_BORDER -state disabled -height 14
pack $p.out -fill both -expand 1 -padx 6 -pady 6
bind $hier_frame.canvas <Configure> {after idle render_hierarchy_treemap}
pack forget $p

# ===== Status Bar =====
label .status.text -text "  AI Digital v$::APP_VERSION | Ready. Type /help for commands." -fg $::C_DIM -bg $::C_PANEL -anchor w -font {Helvetica 9}
label .status.module -text "" -fg $::C_HIGHLIGHT -bg $::C_PANEL -anchor e -font {Helvetica 9}
pack .status.text -side left -fill x; pack .status.module -side right -padx 8

# ===================== Init =====================
debug "GUI built, initializing..."
console_log "AI Digital v$::APP_VERSION" "ok"
console_log "Binary: $::BINARY" ""
console_log "Starting CLI subprocess..." ""
update idletasks

# Open pipe to CLI subprocess
if {[pipe_open]} {
    console_log "CLI subprocess started. Type /help for commands." "ok"
} else {
    console_log "Warning: CLI subprocess not available. Use File > Open Project." "warn"
}

# Set initial page
set_page rtl

after 500 {
    set script_dir [file dirname [info script]]
    set state_path [file join $script_dir "workspace" "default" "exchange" "gui_state.tcl"]
    if {[file exists $state_path]} {
        load_gui_state_file $state_path
    }
}
after 1800 gui_run_autodump

debug "GUI ready"
console_log "Type /help for available commands" ""
