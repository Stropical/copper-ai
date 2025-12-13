#!/bin/bash
# Simple script to run the Python agent with the virtual environment

cd "$(dirname "$0")"

# Check if uv is available and use it (preferred)
if command -v uv &> /dev/null; then
    echo "Using uv to run agent..."
    uv run python main.py
    exit $?
fi

# Fallback to manual venv activation
# Check for .venv first (uv default), then venv
if [ -d ".venv" ]; then
    echo "Activating .venv..."
    source .venv/bin/activate
elif [ -d "venv" ]; then
    echo "Activating venv..."
    source venv/bin/activate
fi

# Run the agent
python main.py

