#!/bin/bash
# AI Digital - Installation Script
# Builds the project from source and installs the binary

set -e

echo "========================================="
echo "  AI Digital v0.6.8 - Installation"
echo "========================================="
echo ""

# Check dependencies
echo "Checking dependencies..."

# Check for Rust
if ! command -v cargo &> /dev/null; then
    echo "Rust is not installed. Installing..."
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    source "$HOME/.cargo/env"
    echo "Rust installed successfully."
fi

# Check for cmake
if ! command -v cmake &> /dev/null; then
    echo "cmake is not installed. Installing..."
    if command -v dnf &> /dev/null; then
        sudo dnf install -y cmake
    elif command -v apt-get &> /dev/null; then
        sudo apt-get update && sudo apt-get install -y cmake
    elif command -v pacman &> /dev/null; then
        sudo pacman -S --noconfirm cmake
    else
        echo "Error: Cannot install cmake. Please install manually."
        exit 1
    fi
fi

# Check for g++
if ! command -v g++ &> /dev/null; then
    echo "g++ is not installed. Installing..."
    if command -v dnf &> /dev/null; then
        sudo dnf install -y gcc-c++
    elif command -v apt-get &> /dev/null; then
        sudo apt-get update && sudo apt-get install -y build-essential
    elif command -v pacman &> /dev/null; then
        sudo pacman -S --noconfirm gcc
    else
        echo "Error: Cannot install g++. Please install manually."
        exit 1
    fi
fi

echo "All dependencies found."
echo ""

# Build
echo "Building AI Digital (release mode)..."
echo "This may take a few minutes on first build..."
echo ""

source "$HOME/.cargo/env"
export RUST_MIN_STACK=16777216
cargo build --release

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo ""
echo "Build successful!"
echo ""

# Create workspace directory
mkdir -p workspace

# Show binary location
BINARY="target/release/ai_digital"
if [ -f "$BINARY" ]; then
    echo "Binary location: $(pwd)/$BINARY"
    echo ""
    echo "To run AI Digital:"
    echo "  ./$BINARY"
    echo ""
    echo "To install system-wide (optional):"
    echo "  sudo cp $BINARY /usr/local/bin/ai_digital"
    echo ""
    echo "Testing installation..."
    if echo '/quit' | timeout 5 ./$BINARY > /dev/null 2>&1; then
        echo "  ✓ Installation test passed"
    else
        echo "  ⚠ Installation test failed (but binary exists)"
    fi
    echo ""
    echo "========================================="
    echo "  Installation complete!"
    echo "========================================="
else
    echo "Error: Binary not found at $BINARY"
    exit 1
fi
