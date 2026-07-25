# AI Digital v0.6.7

AI-driven RTL code generation, simulation, and synthesis tool. Generate Verilog RTL from natural language descriptions, with automated lint checking, simulation, synthesis, formal verification, and AI-powered auto-fix.

## Features

- **Natural Language RTL Generation**: Describe circuit behavior in plain English, LLM generates synthesizable Verilog
- **Automated Testbench & SDC**: LLM generates testbench and SDC constraints alongside RTL
- **Simulation**: Compile and simulate generated RTL with Verilator
- **Yosys Synthesis**: Synthesize to gate-level using CMOS standard cell library
- **Timing Analysis**: Clock frequency scanning, slack calculation, MET/VIO determination
- **Area & Power Reports**: Per-cell-type area and power analysis based on liberty library
- **Formal Verification**: RTL vs gate-level equivalence checking using Yosys equiv_make/equiv_simple
- **Multi-API Support**: Switch between different LLM providers (DeepSeek, OpenAI, Ollama, etc.)
- **Project Management**: Organized workspace with src/, tb/, sdc/, sim/, syn/, formal/, history/ directories
- **History Tracking**: Save snapshots of each iteration for comparison
- **AI Auto-Fix**: Automatically sends errors to LLM and retries up to 3 times
- **Interactive CLI**: Claude Code-style conversational interface with tab completion

## File Structure

```
project/
├── Cargo.toml                  # Rust project config (ai_digital v0.6.7)
├── build.rs                    # Build script (compiles C++ engine via CMake)
├── config.yaml                 # Multi-API LLM configuration (auto-created on first run)
├── README.md                   # This file
│
├── engine/                     # C++ engine (references Verilator/Yosys source patterns)
│   ├── CMakeLists.txt          # CMake build configuration
│   ├── include/
│   │   ├── rtl_engine.h        # C API header (exposed to Rust via FFI)
│   │   └── yosys_wrapper.h     # Yosys wrapper header
│   └── src/
│       ├── rtlil.h             # RTLIL data structures (ref: Yosys kernel/rtlil.h)
│       ├── rtlil.cpp           # RTLIL implementation
│       ├── verilog_parser.h    # Verilog parser header
│       ├── verilog_parser.cpp  # Verilog parser (ref: Yosys frontends/verilog/)
│       ├── lint_check.h        # Lint check header
│       ├── lint_check.cpp      # Lint checker (ref: Verilator V3Broken.cpp etc.)
│       ├── synth_passes.h      # Synthesis passes header
│       ├── synth_passes.cpp    # Synthesis engine (ref: Yosys passes/synth.cc)
│       ├── timing_est.h        # Timing estimation header
│       ├── timing_est.cpp      # Timing estimator with CMOS liberty cell data
│       ├── timing_graph.h      # Timing graph header
│       ├── timing_graph.cpp    # Timing graph implementation
│       ├── yosys_core.h        # Yosys core header
│       ├── yosys_core.cpp      # Yosys-inspired synthesis engine
│       ├── yosys_wrapper.h     # Yosys wrapper header
│       ├── yosys_wrapper.cpp   # Yosys wrapper implementation
│       └── rtl_engine.cpp      # C API implementation (timing, SDC, clock scan)
│
├── libs/
│   └── cmos_cells.lib          # CMOS standard cell library (BUF, NOT, NAND, NOR, AND, OR, XOR, MUX, DFF, DFFSR)
│
└── src/                        # Rust source code
    ├── main.rs                 # CLI entry point (clap, initial setup, multi-API config)
    ├── engine/
    │   ├── mod.rs              # Safe Rust wrappers for C++ engine
    │   └── ffi.rs              # Raw C++ FFI bindings
    ├── llm/
    │   └── mod.rs              # LLM HTTP client (OpenAI-compatible API, multi-API support)
    └── repl/
        └── mod.rs              # Interactive REPL (CLI commands, project management, formal verification)
```

## Dependencies

### Build Requirements

- **Rust toolchain** (rustc, cargo) - https://rustup.rs
- **CMake** 3.15+
- **GCC/G++** with C++17 support
- **make**

### Runtime Requirements

- **Verilator** - for RTL simulation
  - Check path: `which verilator` or set `VERILATOR_PATH` env var
- **Yosys** - for logic synthesis and formal verification
  - Check path: `which yosys` or set `YOSYS_PATH` env var

## Build from Source

### Quick Start

```bash
# Clone the repository
cd /home/lumouren/software/ai_rtl_sim

# Build the project
cd project
cargo build

# The binary is at:
# → target/debug/ai_digital
```

### Detailed Build Steps

#### 1. Install Prerequisites

```bash
# Install Rust toolchain (if not installed)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# Install CMake (if not installed)
# On Ubuntu/Debian:
sudo apt-get install cmake

# On Fedora/RHEL:
sudo dnf install cmake

# Install GCC/G++ (if not installed)
# On Ubuntu/Debian:
sudo apt-get install build-essential

# On Fedora/RHEL:
sudo dnf install gcc gcc-c++ make
```

#### 2. Build the Project

```bash
cd /home/lumouren/software/ai_rtl_sim/project

# Development build (debug mode, faster compilation)
cargo build

# Release build (optimized, slower compilation but faster execution)
cargo build --release
```

#### 3. Install (Optional)

```bash
# Install to system path
cargo install --path .

# Or copy manually
cp target/release/ai_digital /usr/local/bin/
```

#### 4. Verify Installation

```bash
# Check version
./target/debug/ai_digital --version

# Check tool availability
./target/debug/ai_digital --check
```

### Build Output

After successful build, you will find:

```
project/target/
├── debug/
│   ├── ai_digital              # Debug binary
│   └── build/
│       └── rtl_engine-xxx/
│           └── out/
│               └── librtl_engine.a  # C++ engine library
└── release/
    ├── ai_digital              # Release binary
    └── build/
        └── rtl_engine-xxx/
            └── out/
                └── librtl_engine.a  # C++ engine library (optimized)
```

### C++ Engine Build

The C++ engine is automatically built by the Rust build script (`build.rs`). If you need to build it manually:

```bash
cd /home/lumouren/software/ai_rtl_sim/project/engine

# Create build directory
mkdir -p build && cd build

# Configure with CMake
cmake ..

# Build
make -j$(nproc)

# The library is at:
# → build/librtl_engine.a
```

### Troubleshooting

#### CMake not found

```bash
# Install CMake
# Ubuntu/Debian:
sudo apt-get install cmake

# Fedora/RHEL:
sudo dnf install cmake
```

#### C++ compiler not found

```bash
# Install GCC/G++
# Ubuntu/Debian:
sudo apt-get install build-essential

# Fedora/RHEL:
sudo dnf install gcc gcc-c++ make
```

#### Rust toolchain not found

```bash
# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

#### Verilator/Yosys not found

```bash
# The tool will work without Verilator/Yosys
# but simulation and synthesis features will be limited

# To install Verilator (optional):
# Ubuntu/Debian:
sudo apt-get install verilator

# To install Yosys (optional):
# Ubuntu/Debian:
sudo apt-get install yosys
```

## Configuration

### First Run (Initial Setup)

When you run `ai_digital` for the first time without a config file, it enters **initial setup mode**:

```
╔══════════════════════════════════════════╗
║       AI Digital - Initial Setup         ║
╚══════════════════════════════════════════╝

  No config file found at: ./config.yaml
  Let's set up your configuration.

  → LLM Base URL [http://localhost:11434/v1]: https://api.deepseek.com
  → API Key [(empty)]: sk-xxx
  → Model [qwen2.5-coder:7b]: deepseek-v4-flash
  → Temperature [0.3]:
  → Max tokens [8192]: 4096
  → Timeout (seconds) [120]:

  ✓ Config saved to: ./config.yaml
```

### Config File Format (config.yaml)

```yaml
# Multiple API profiles - first one is default
apis:
  deepseek:
    base_url: "https://api.deepseek.com"
    api_key: "sk-xxx"
    model: "deepseek-v4-flash"
    temperature: 0.3
    max_tokens: 8192
    timeout: 120
  ollama_local:
    base_url: "http://localhost:11434/v1"
    api_key: ""
    model: "qwen2.5-coder:7b"
    temperature: 0.3
    max_tokens: 4096
    timeout: 120
  openai:
    base_url: "https://api.openai.com/v1"
    api_key: "sk-xxx"
    model: "gpt-4o"
    temperature: 0.3
    max_tokens: 8192
    timeout: 120

# Active API alias (must match a key in apis)
active_api: "deepseek"
```

### Switching APIs

```bash
# List available APIs
/api

# Switch to a different API
/api ollama_local
```

## Usage

### 1. Interactive Mode (Default)

```bash
./target/debug/ai_digital
```

Example session:

```
╔══════════════════════════════════════════╗
║       AI Digital v0.6.7                   ║
║  Generate · Lint · Synthesize · Optimize ║
╚══════════════════════════════════════════╝

  ● Engine: rtl-engine 0.6.7
  ● LLM:    [deepseek] Provider: OpenAI-compatible, Model: deepseek-v4-flash

ai_digital ▸ write an 8-bit ALU with add, sub, and, or, xor

  ✓ RTL: src/alu_8bit.v
  ✓ Testbench: tb/alu_8bit_tb.v
  ✓ SDC: sdc/alu_8bit.sdc
  ✓ Parsed OK
  ✓ Lint passed
  ✓ Simulation PASSED
  ✓ Synthesis done

  Area Report: ...
  Power Report: ...

  ✓ Scanned 100-1320 MHz

  Timing at constraint (100 MHz): ...
  Max frequency timing (1320 MHz): ...

  ✓ Formal Verification: PASS - RTL and gate-level netlist are equivalent

  ✓ Snapshot: history/v1
```

### 2. Interactive Commands

| Command | Description |
|---------|-------------|
| `/lint [module]` | Run lint check on current RTL |
| `/synth [module]` | Run synthesis on current RTL |
| `/stats [module]` | Show synthesis statistics |
| `/area` | Show area report |
| `/power` | Show power report |
| `/set freq <MHz>` | Set constraint frequency |
| `/export [module]` | Export synthesized Verilog |
| `/api` | Show/switch API configuration |
| `/projects` | List past projects |
| `/load <project>` | Load a past project |
| `/modules` | Show loaded modules |
| `/config` | Show configuration |
| `/reset` | Reset design state |
| `/clean` | Clean workspace files |
| `/clear` | Clear screen |
| `/history` | Show conversation history |
| `/help` | Show help |
| `/quit` | Exit |

Tab completion is supported for all commands.

### 3. Natural Language Input

```
ai_digital ▸ write an 8-bit counter with enable and reset
ai_digital ▸ optimize area, reduce cell count
ai_digital ▸ add pipeline stages for better timing
ai_digital ▸ write a UART transmitter
```

### 4. Non-Interactive Mode

```bash
# Generate RTL from description
./target/debug/ai_digital -d "4-bit counter with enable and async reset"

# Specify module name
./target/debug/ai_digital -d "FIFO buffer" --top my_fifo

# Generate + lint + synthesize
./target/debug/ai_digital -d "4-bit counter" --lint --synthesize
```

### 5. Check Tool Availability

```bash
./target/debug/ai_digital --check
```

## Workspace Structure

Each generated RTL module creates a project directory:

```
workspace/
  <module_name>/
    src/           # RTL source files (.v, .sv)
    tb/            # Testbenches (*_tb.v)
    sdc/           # SDC constraint files (.sdc)
    sim/           # Simulation outputs (VCD, reports)
    syn/           # Synthesis outputs (gate-level netlists, reports)
    formal/        # Formal verification reports
    history/       # Version snapshots (v1/, v2/, ...)
```

## Source Code References

This tool's C++ engine references design patterns from the following open source projects:

| Component | Reference | Description |
|-----------|-----------|-------------|
| RTLIL data structures | Yosys `kernel/rtlil.h` | Design/Module/Wire/Cell core structures |
| Verilog parser | Yosys `frontends/verilog/verilog_parser.y` | Lexical analysis and parsing patterns |
| Lint checking | Verilator `V3Broken.cpp`, `V3Undriven.cpp`, `V3Width.cpp` | Signal connectivity, undriven, width checks |
| Synthesis passes | Yosys `passes/synth.cc`, `passes/opt/` | Process extraction, optimization, tech mapping |
| Formal verification | SymbiYosys `sbysrc/sby_engine_smtbmc.py` | Equivalence checking patterns |
| Timing analysis | OpenSTA concepts | Setup slack, wire load model, clock scanning |

Reference, not copy: Design ideas and algorithm patterns extracted from the above sources, rewritten as independent implementations.
