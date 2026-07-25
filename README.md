# AI Digital

AI Digital is a local RTL design flow tool that combines a native Rust/C++ implementation with an LLM-driven control loop.  
It targets the front-end digital design flow from natural-language intent to RTL, simulation, synthesis, timing, area, power, and formal verification.

## Features

- Natural-language driven RTL workflow
- Interactive CLI with command completion
- GUI for RTL, waveform, synthesis, timing, formal, and summary views
- Native simulation flow with waveform generation and parsing
- Native gate-level netlist rendering and analysis views
- Synthesis, timing, area, and power reporting
- Multi-corner timing and power analysis
- Formal RTL vs gate-level equivalence checking
- Per-project workspace isolation and project switching
- LLM-centered iteration flow: analysis, decision, retry, and abort

## Repository Layout

```text
project/
├── src/              # Rust application code
├── engine/           # Native C++ engine
├── libs/             # Liberty libraries and technology data
├── test_circuits/    # Example circuits
├── gui.tcl           # Tk GUI
├── install.sh        # Build/install helper
└── workspace/        # Per-project runtime data (generated, not committed)
```

## Requirements

### Build dependencies

- Rust toolchain (`cargo`, `rustc`)
- CMake
- GCC / G++ with C++17 support
- `make`

### Runtime dependencies

- Tcl/Tk (`wish`) for the GUI
- Network access to your configured LLM API endpoint if you use LLM-driven flow steps

### Supported platforms

- Linux
- Primarily tested for RHEL 9/10 and Fedora-family environments

## Installation

### 1. Clone and enter the project

```bash
cd /path/to/ai_rtl_sim/project
```

### 2. Install build dependencies

Fedora / RHEL:

```bash
sudo dnf install gcc gcc-c++ make cmake tcl tk
```

Debian / Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake tcl tk
```

Install Rust if needed:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

### 3. Build

Release build is recommended:

```bash
cargo build --release
```

Binary path:

```bash
./target/release/ai_digital
```

### 4. Optional install script

The repository also provides:

```bash
./install.sh
```

## Configuration

The CLI reads `config.yaml` from the project directory.

The config should point to your LLM endpoint and model. The current code supports OpenAI-compatible chat endpoints, including common `/chat/completions` and `/v1/chat/completions` layouts.

Typical fields include:

- API type
- base URL
- API key
- model name
- default frequency constraint
- library selection

Do not commit your personal `config.yaml`.

## Usage

### CLI

Start the interactive CLI:

```bash
./target/release/ai_digital --config config.yaml
```

Common commands:

```text
/project list
/project switch <name>
/project open <path>
/lint <module>
/sim <module>
/synth <module>
/timing <module>
/power
/formal <module>
/full <module>
/quit
```

`Tab` completion is supported for commands.

### GUI

Launch the GUI:

```bash
wish gui.tcl
```

The GUI provides:

- RTL and testbench view
- Simulation log and waveform view
- Synthesis schematic view
- Timing path visualization
- Power and area reports
- Formal equivalence report and graph
- Design summary and hierarchy view

## Typical flow

1. Open or switch to a project.
2. Load or generate RTL and testbench.
3. Run simulation.
4. Run synthesis.
5. Inspect timing, area, and power.
6. Run formal verification.
7. Use `/full <module>` for the integrated flow when the LLM path is configured.

## Notes

- Release builds are strongly recommended for real runs and GUI integration.
- If the LLM is unavailable during an LLM-required flow, the tool aborts instead of silently skipping that stage.
- Runtime project data is stored under `workspace/`.

## License

Add your project license here if you plan to publish the repository.
