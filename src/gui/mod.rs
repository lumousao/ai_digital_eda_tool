/**
 * GUI Module - Tcl/Tk Desktop GUI Launcher
 *
 * Professional EDA-style interface using Tcl/Tk (wish).
 * Compatible with all Unix desktops and SSH X-forwarding.
 * Requires: tclsh/wish (installed on virtually all Linux systems)
 *
 * Usage: ai_digital -gui
 * This launches the Tcl/Tk GUI script.
 */

use std::process::Command;
use std::path::PathBuf;

/// Run the Tcl/Tk GUI application
pub fn run() -> Result<(), String> {
    // Find the gui.tcl script
    let script_path = find_gui_script()?;

    println!("╔══════════════════════════════════════════╗");
    println!("║   AI Digital v0.7.0 — GUI Mode          ║");
    println!("╚══════════════════════════════════════════╝");
    println!();
    println!("  Launching Tcl/Tk GUI...");

    // Try wish first, then tclsh
    let wish = find_wish().ok_or_else(|| {
        "Tcl/Tk not found. Install it with: Fedora/RHEL: dnf install tcl tk; Debian/Ubuntu: apt-get install tcl tk".to_string()
    })?;

    println!("  Using: {}", wish);
    println!("  Script: {}", script_path.display());
    println!();

    let mut cmd = Command::new(&wish);
    cmd.arg(&script_path)
        .current_dir(script_path.parent().unwrap_or_else(|| std::path::Path::new(".")))
        .env("TK_USE_INPUT_METHODS", "1");

    if let Some(lang) = std::env::var_os("LANG") {
        if std::env::var_os("LC_CTYPE").is_none() {
            cmd.env("LC_CTYPE", lang);
        }
    }

    let detected_im = detect_input_method();
    if std::env::var_os("XMODIFIERS").is_none() {
        if let Some(im) = std::env::var_os("GTK_IM_MODULE") {
            let value = format!("@im={}", im.to_string_lossy());
            cmd.env("XMODIFIERS", value);
        } else if let Some(im) = std::env::var_os("QT_IM_MODULE") {
            let value = format!("@im={}", im.to_string_lossy());
            cmd.env("XMODIFIERS", value);
        } else if let Some(im) = detected_im.as_deref() {
            cmd.env("XMODIFIERS", format!("@im={}", im));
        }
    }
    if std::env::var_os("GTK_IM_MODULE").is_none() {
        if let Some(im) = detected_im.as_deref() {
            cmd.env("GTK_IM_MODULE", im);
        }
    }
    if std::env::var_os("QT_IM_MODULE").is_none() {
        if let Some(im) = detected_im.as_deref() {
            cmd.env("QT_IM_MODULE", im);
        }
    }
    if std::env::var_os("SDL_IM_MODULE").is_none() {
        if let Some(im) = detected_im.as_deref() {
            cmd.env("SDL_IM_MODULE", im);
        }
    }

    let status = cmd
        .status()
        .map_err(|e| format!("Failed to launch GUI: {}", e))?;

    if !status.success() {
        return Err(format!("GUI exited with code: {:?}", status.code()));
    }

    Ok(())
}

/// Find the gui.tcl script relative to the binary
fn find_gui_script() -> Result<PathBuf, String> {
    // Try relative to binary location
    if let Ok(exe) = std::env::current_exe() {
        let exe_dir = exe.parent().unwrap();
        for candidate in &[
            exe_dir.join("gui.tcl"),
            exe_dir.join("../gui.tcl"),
            exe_dir.join("../../gui.tcl"),
            PathBuf::from("gui.tcl"),
            PathBuf::from("project/gui.tcl"),
            PathBuf::from("../gui.tcl"),
        ] {
            if candidate.exists() {
                return Ok(candidate.canonicalize().unwrap_or(candidate.clone()));
            }
        }
    }

    // Fallback to project/gui.tcl
    let cwd = std::env::current_dir().unwrap_or_default();
    for candidate in &[
        cwd.join("gui.tcl"),
        cwd.join("project/gui.tcl"),
    ] {
        if candidate.exists() {
            return Ok(candidate.clone());
        }
    }

    Err("gui.tcl not found. Ensure the script is in the project directory.".to_string())
}

/// Find Tcl/Tk launcher binary.
fn find_wish() -> Option<String> {
    for candidate in &["wish", "wish9.0", "wish8.7", "wish8.6"] {
        if let Ok(output) = Command::new("which").arg(candidate).output() {
            if output.status.success() {
                let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
                if !path.is_empty() {
                    return Some(path);
                }
            }
        }
    }
    for candidate in &["tclsh", "tclsh9.0", "tclsh8.7", "tclsh8.6"] {
        if let Ok(output) = Command::new("which").arg(candidate).output() {
            if output.status.success() {
                let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
                if !path.is_empty() {
                    return Some(path);
                }
            }
        }
    }
    for path in &["/usr/bin/tclsh", "/usr/local/bin/tclsh"] {
        if std::path::Path::new(path).exists() {
            return Some(path.to_string());
        }
    }
    None
}

fn detect_input_method() -> Option<String> {
    if let Some(value) = std::env::var_os("GTK_IM_MODULE") {
        let im = value.to_string_lossy().trim().to_string();
        if !im.is_empty() {
            return Some(im);
        }
    }
    if let Some(value) = std::env::var_os("QT_IM_MODULE") {
        let im = value.to_string_lossy().trim().to_string();
        if !im.is_empty() {
            return Some(im);
        }
    }
    for candidate in ["fcitx5", "fcitx", "ibus"] {
        if let Ok(output) = Command::new("which").arg(candidate).output() {
            if output.status.success() {
                return Some(candidate.to_string());
            }
        }
    }
    None
}
