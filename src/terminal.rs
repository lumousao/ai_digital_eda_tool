/**
 * Terminal Manager — Raw-mode input + Tab completion + dynamic spinner
 *
 * Idle:       ai_digital [project] ▸ _        (raw mode, manual echo)
 * Running:    ⠋ Executing: synthesis...        (spinner on current line)
 *             [output scrolls above]
 */

use std::io::{self, Write, stdout};
use std::sync::{Mutex, atomic::{AtomicBool, Ordering}};
use std::thread;
use std::time::Duration;

/// Force plain (non-TTY) mode — used by `--pipe` mode where the GUI embeds the CLI.
/// When true, all output is plain text, no ANSI codes, no raw mode, no spinners.
static FORCE_PLAIN: AtomicBool = AtomicBool::new(false);

/// Enable forced plain mode (pipe mode).
pub fn set_force_plain(plain: bool) {
    FORCE_PLAIN.store(plain, Ordering::SeqCst);
}

pub fn is_tty() -> bool {
    if FORCE_PLAIN.load(Ordering::SeqCst) { return false; }
    unsafe { libc::isatty(1) != 0 }
}

// ── Spinner ──
const SPINNER: &[char] = &['⠋', '⠙', '⠹', '⠸', '⠼', '⠴', '⠦', '⠧', '⠇', '⠏'];

static SPIN_DESC: Mutex<String> = Mutex::new(String::new());
static SPIN_ACTIVE: AtomicBool = AtomicBool::new(false);

fn spinner_start(desc: &str) {
    if !is_tty() { println!("  > {}", desc); return; }
    *SPIN_DESC.lock().unwrap() = desc.to_string();

    if SPIN_ACTIVE.swap(true, Ordering::SeqCst) {
        // Already active — just updated description, no need to start new thread
        return;
    }

    // First time: start a single background spinner thread
    let mut out = stdout();
    let _ = out.write_all(b"\n");
    let _ = out.flush();

    thread::spawn(move || {
        let mut tick = 0usize;
        loop {
            thread::sleep(Duration::from_millis(100));
            if !SPIN_ACTIVE.load(Ordering::SeqCst) { break; }
            tick += 1;
            let sp = SPINNER[tick % 10];
            let desc = SPIN_DESC.lock().unwrap().clone();
            let mut out = stdout();
            let _ = out.write_all(
                format!(
                    "\r\x1b[38;5;33m┃\x1b[0m \x1b[38;5;81m{}\x1b[0m \x1b[1;37m{}\x1b[0m\x1b[K",
                    sp, desc
                )
                .as_bytes(),
            );
            let _ = out.flush();
        }
    });
}

fn spinner_update(desc: &str) {
    *SPIN_DESC.lock().unwrap() = desc.to_string();
}

fn spinner_stop_and_wait() {
    if !is_tty() { SPIN_ACTIVE.store(false, Ordering::SeqCst); return; }
    SPIN_ACTIVE.store(false, Ordering::SeqCst);
    // Wait 200ms for spinner thread to notice and exit
    thread::sleep(Duration::from_millis(200));
    // Clear spinner line
    let mut out = stdout();
    let _ = out.write_all(b"\r\x1b[K");
    let _ = out.flush();
}

pub fn status_start(desc: &str) { spinner_start(desc); }
pub fn status_update(desc: &str) { spinner_update(desc); }

pub fn status_stop(ok: bool, label: &str) {
    spinner_stop_and_wait();
    if !label.is_empty() {
        if !is_tty() {
            println!("{} {}", if ok { "✓" } else { "✗" }, label);
        } else if ok {
            println!("\x1b[38;5;33m┃\x1b[0m \x1b[38;5;82m✓\x1b[0m {}", label);
        } else {
            println!("\x1b[38;5;33m┃\x1b[0m \x1b[38;5;196m✗\x1b[0m {}", label);
        }
    }
}

// ── Output ──
pub fn term_println(line: &str) {
    if !is_tty() { println!("{}", line); return; }
    if SPIN_ACTIVE.load(Ordering::SeqCst) {
        let mut out = stdout();
        let _ = out.write_all(b"\r\x1b[K"); // clear spinner line
        let _ = out.write_all(line.as_bytes());
        let _ = out.write_all(b"\n");
        let _ = out.flush();
    } else {
        println!("{}", line);
    }
}

pub fn term_print(s: &str) {
    if !is_tty() { print!("{}", s); return; }
    let mut out = stdout();
    if SPIN_ACTIVE.load(Ordering::SeqCst) {
        let _ = out.write_all(b"\r\x1b[K");
    }
    let _ = out.write_all(s.as_bytes());
    let _ = out.flush();
}

// ── Raw mode ──
static ORIG_TERMIOS: std::sync::OnceLock<Mutex<libc::termios>> = std::sync::OnceLock::new();
static RAW_ACTIVE: AtomicBool = AtomicBool::new(false);

fn raw_on() {
    if RAW_ACTIVE.swap(true, Ordering::SeqCst) { return; }
    unsafe {
        let mut raw: libc::termios = std::mem::zeroed();
        libc::tcgetattr(0, &mut raw);
        ORIG_TERMIOS.get_or_init(|| Mutex::new(raw));
        raw.c_lflag &= !(libc::ECHO | libc::ICANON);
        raw.c_cc[libc::VMIN] = 1;
        raw.c_cc[libc::VTIME] = 0;
        libc::tcsetattr(0, libc::TCSANOW, &raw);
    }
}

fn raw_off() {
    if !RAW_ACTIVE.swap(false, Ordering::SeqCst) { return; }
    if let Some(orig) = ORIG_TERMIOS.get() {
        unsafe { libc::tcsetattr(0, libc::TCSANOW, &*orig.lock().unwrap()); }
    }
}

pub fn term_init() -> bool { is_tty() }
pub fn term_shutdown() { raw_off(); }

fn read_stdin_byte() -> io::Result<u8> {
    let mut byte = [0u8; 1];
    let n = unsafe { libc::read(0, byte.as_mut_ptr() as *mut libc::c_void, 1) };
    if n <= 0 {
        Err(io::Error::new(io::ErrorKind::UnexpectedEof, "EOF"))
    } else {
        Ok(byte[0])
    }
}

fn shared_prefix(options: &[String]) -> String {
    let Some(first) = options.first() else {
        return String::new();
    };
    let mut prefix = first.clone();
    for option in &options[1..] {
        let shared: String = prefix
            .chars()
            .zip(option.chars())
            .take_while(|(a, b)| a == b)
            .map(|(ch, _)| ch)
            .collect();
        prefix = shared;
        if prefix.is_empty() {
            break;
        }
    }
    prefix
}

fn redraw_readline(prompt: &str, line: &str) -> io::Result<()> {
    let mut out = stdout();
    out.write_all(b"\r\x1b[2K")?;
    out.write_all(prompt.as_bytes())?;
    out.write_all(line.as_bytes())?;
    out.write_all(b"\x1b[K")?;
    out.flush()?;
    Ok(())
}

// ── Line Input ──
pub fn readline(prompt: &str, completions: &dyn Fn(&str) -> Vec<String>) -> io::Result<String> {
    if !is_tty() {
        let mut line = String::new();
        match io::stdin().read_line(&mut line) {
            Ok(0) => return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "EOF")),
            Ok(_) => return Ok(line.trim().to_string()),
            Err(e) => return Err(e),
        }
    }

    raw_on();
    let mut line = String::new();
    redraw_readline(prompt, &line)?;

    loop {
        let byte = match read_stdin_byte() {
            Ok(byte) => byte,
            Err(err) => {
                raw_off();
                return Err(err);
            }
        };

        match byte {
            10 | 13 => { // Enter
                redraw_readline(prompt, &line)?;
                let _ = stdout().write_all(b"\r\n");
                let _ = stdout().flush();
                break;
            }
            127 | 8 => { // Backspace
                if !line.is_empty() {
                    line.pop();
                }
                let _ = redraw_readline(prompt, &line);
            }
            3 => { // Ctrl-C
                let _ = redraw_readline(prompt, &line);
                let _ = stdout().write_all(b"^C\r\n");
                let _ = stdout().flush();
                raw_off();
                return Err(io::Error::new(io::ErrorKind::Interrupted, "Ctrl-C"));
            }
            4 if line.is_empty() => { // Ctrl-D on empty
                raw_off();
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "EOF"));
            }
            9 => { // Tab
                let suggestions = if line.starts_with('/') {
                    completions(&line)
                } else {
                    Vec::new()
                };
                if !suggestions.is_empty() {
                    let common = shared_prefix(&suggestions);
                    if common.len() > line.len() {
                        line = common;
                    } else {
                        line = suggestions[0].clone();
                    }
                    let _ = redraw_readline(prompt, &line);
                }
            }
            27 => {
                let next = match read_stdin_byte() {
                    Ok(byte) => byte,
                    Err(_) => continue,
                };
                if next != b'[' {
                    continue;
                }
                let code = match read_stdin_byte() {
                    Ok(byte) => byte,
                    Err(_) => continue,
                };
                match code {
                    b'A' => {}
                    b'B' => {}
                    _ => {}
                }
            }
            32..=126 => {
                line.push(byte as char);
                let _ = redraw_readline(prompt, &line);
            }
            _ => {
                // Multi-byte UTF-8 character handling (Chinese, etc.)
                let first = byte;
                // Determine UTF-8 sequence length from leading byte
                let seq_len = if first & 0x80 == 0 {
                    1 // ASCII — already handled above
                } else if first & 0xE0 == 0xC0 {
                    2 // 2-byte UTF-8
                } else if first & 0xF0 == 0xE0 {
                    3 // 3-byte UTF-8 (Chinese characters, most CJK)
                } else if first & 0xF8 == 0xF0 {
                    4 // 4-byte UTF-8 (rare supplementary chars)
                } else {
                    0 // invalid leading byte
                };

                if seq_len > 1 {
                    let mut utf8_buf = vec![first];
                    let mut valid = true;
                    for _ in 1..seq_len {
                        let b = match read_stdin_byte() {
                            Ok(byte) => byte,
                            Err(_) => { valid = false; break; }
                        };
                        // Validate continuation byte: must be 10xxxxxx
                        if b & 0xC0 != 0x80 {
                            valid = false;
                            break;
                        }
                        utf8_buf.push(b);
                    }
                    if valid {
                        if let Ok(s) = std::str::from_utf8(&utf8_buf) {
                            line.push_str(s);
                            let _ = redraw_readline(prompt, &line);
                        }
                    }
                }
            }
        }
    }

    raw_off();
    Ok(line)
}
