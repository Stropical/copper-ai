#!/bin/bash
# Debug script for KiCad crash

cd /Users/ethanmarreel/Documents/GitHub/kicad-source-mirror/build/release

echo "=== Running KiCad with lldb to catch crash ==="
echo ""

# Run with lldb and catch exceptions
lldb ./kicad/KiCad.app/Contents/MacOS/kicad <<EOF
# Set breakpoints on exceptions
breakpoint set -E C++
breakpoint set -E ObjC

# Run and catch
run

# When it crashes, show backtrace
bt

# Show all threads
thread list

# Show local variables
frame variable

# Continue to see if it crashes again
continue
EOF

