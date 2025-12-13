#!/bin/bash
# Simple script to run the Python agent with the virtual environment

cd "$(dirname "$0")"

# Activate virtual environment if it exists
if [ -d "venv" ]; then
    source venv/bin/activate
fi

# Run the agent
python main.py

