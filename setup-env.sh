#!/bin/bash
# ⚠️  NOTE: This script is for Linux, NOT macOS!
# 
# For macOS, use the unified script instead:
#   ./dev-build.sh
#
# Source this file to set up the environment for running KiCad from ~/bin/kicad
# Usage: source setup-env.sh

export LD_LIBRARY_PATH="$HOME/bin/kicad/lib:$LD_LIBRARY_PATH"
export PATH="$HOME/bin/kicad/bin:$PATH"

echo "KiCad environment configured:"
echo "  LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
echo "  PATH: $PATH"
