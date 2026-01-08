#!/bin/bash
# dev-all.sh
# Opens 4 Git Bash terminal panes in a 2x2 grid and runs dev services.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

WEBSITE_DIR="$SCRIPT_DIR/PRIVATE/copper-ai-private/website"
RAG_DIR="$SCRIPT_DIR/PRIVATE/copper-ai-private/pcb_agent"
AGENT_PANEL_DIR="$SCRIPT_DIR/eeschema/widgets/agent_panel"
ROUTER_SERVER_DIR="$SCRIPT_DIR/PRIVATE/copper-ai-private/router_server"

# Validate paths
for p in "$WEBSITE_DIR" "$RAG_DIR" "$AGENT_PANEL_DIR" "$ROUTER_SERVER_DIR"; do
    if [ ! -d "$p" ]; then
        echo "Missing path: $p"
        exit 1
    fi
done

# On Windows (Git Bash), venv uses Scripts folder
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
    VENV_ACTIVATE="$RAG_DIR/.venv/Scripts/activate"
else
    VENV_ACTIVATE="$RAG_DIR/.venv/bin/activate"
fi

# Ensure rag_server venv exists
if [ ! -f "$VENV_ACTIVATE" ]; then
    if ! command -v uv &> /dev/null; then
        echo "Missing uv. Install uv before continuing."
        exit 1
    fi
    echo "Missing venv. Creating with uv in $RAG_DIR/.venv..."
    pushd "$RAG_DIR" > /dev/null
    uv sync
    popd > /dev/null
fi

# Detect platform and terminal
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
    # Windows with Windows Terminal + Git Bash
    
    # Find Git Bash executable
    GIT_BASH=""
    if [ -f "/c/Program Files/Git/bin/bash.exe" ]; then
        GIT_BASH="C:/Program Files/Git/bin/bash.exe"
    elif [ -f "/c/Program Files (x86)/Git/bin/bash.exe" ]; then
        GIT_BASH="C:/Program Files (x86)/Git/bin/bash.exe"
    fi
    
    if [ -z "$GIT_BASH" ]; then
        echo "Git Bash not found at expected locations."
        exit 1
    fi
    
    # Convert paths to Windows format
    WEBSITE_DIR_WIN=$(cygpath -w "$WEBSITE_DIR")
    RAG_DIR_WIN=$(cygpath -w "$RAG_DIR")
    AGENT_PANEL_DIR_WIN=$(cygpath -w "$AGENT_PANEL_DIR")
    ROUTER_SERVER_DIR_WIN=$(cygpath -w "$ROUTER_SERVER_DIR")
    
    # Create temporary startup scripts for each pane
    mkdir -p "$SCRIPT_DIR/.dev-scripts"
    
    # Script 1: website
    cat > "$SCRIPT_DIR/.dev-scripts/1-website.sh" << 'SCRIPT1'
cd "$(dirname "$0")/../PRIVATE/copper-ai-private/website"
npm install && npm run dev
exec bash
SCRIPT1

    # Script 2: rag_server
    cat > "$SCRIPT_DIR/.dev-scripts/2-rag.sh" << 'SCRIPT2'
cd "$(dirname "$0")/../PRIVATE/copper-ai-private/pcb_agent"
source .venv/Scripts/activate
python rag_server.py
exec bash
SCRIPT2

    # Script 3: agent_panel
    cat > "$SCRIPT_DIR/.dev-scripts/3-agent.sh" << 'SCRIPT3'
cd "$(dirname "$0")/../eeschema/widgets/agent_panel"
npm install && npm run dev
exec bash
SCRIPT3

    # Script 4: router_server
    cat > "$SCRIPT_DIR/.dev-scripts/4-router.sh" << 'SCRIPT4'
cd "$(dirname "$0")/../PRIVATE/copper-ai-private/router_server"
# Add cargo to PATH (standard Windows rustup location)
export PATH="$HOME/.cargo/bin:$USERPROFILE/.cargo/bin:/c/Users/$USER/.cargo/bin:$PATH"
cargo run
exec bash
SCRIPT4

    # Convert script paths to Windows format
    SCRIPT1_WIN=$(cygpath -w "$SCRIPT_DIR/.dev-scripts/1-website.sh")
    SCRIPT2_WIN=$(cygpath -w "$SCRIPT_DIR/.dev-scripts/2-rag.sh")
    SCRIPT3_WIN=$(cygpath -w "$SCRIPT_DIR/.dev-scripts/3-agent.sh")
    SCRIPT4_WIN=$(cygpath -w "$SCRIPT_DIR/.dev-scripts/4-router.sh")
    
    # Get Git Bash path in Windows format
    GIT_BASH_WIN="C:\\Program Files\\Git\\bin\\bash.exe"
    
    # Launch Windows Terminal with 4 Git Bash panes
    # We use bash.exe explicitly with the script as argument
    wt.exe new-tab --title "website" "$GIT_BASH_WIN" --login "$SCRIPT1_WIN" \; \
        split-pane -V --title "agent_panel" "$GIT_BASH_WIN" --login "$SCRIPT3_WIN" \; \
        move-focus left \; \
        split-pane -H --title "rag_server" "$GIT_BASH_WIN" --login "$SCRIPT2_WIN" \; \
        move-focus right \; \
        split-pane -H --title "router_server" "$GIT_BASH_WIN" --login "$SCRIPT4_WIN"
    
    echo "Windows Terminal opened with 4 Git Bash panes running dev services."
elif command -v wt.exe &> /dev/null; then
    # Non-Git-Bash Windows environment but wt.exe available
    echo "Please run this script from Git Bash."
    exit 1
elif command -v tmux &> /dev/null; then
    # Linux/macOS with tmux
    SESSION="copper-dev"
    
    # Kill existing session if present
    tmux kill-session -t "$SESSION" 2>/dev/null
    
    # Create new session with first pane (website)
    tmux new-session -d -s "$SESSION" -n "dev"
    tmux send-keys -t "$SESSION" "$CMD1" Enter
    
    # Split vertically for agent_panel (right side)
    tmux split-window -h -t "$SESSION"
    tmux send-keys -t "$SESSION" "$CMD3" Enter
    
    # Go back to left pane and split horizontally for rag_server
    tmux select-pane -t "$SESSION:0.0"
    tmux split-window -v -t "$SESSION"
    tmux send-keys -t "$SESSION" "$CMD2" Enter
    
    # Go to right pane and split horizontally for router_server
    tmux select-pane -t "$SESSION:0.2"
    tmux split-window -v -t "$SESSION"
    tmux send-keys -t "$SESSION" "$CMD4" Enter
    
    # Select first pane and attach
    tmux select-pane -t "$SESSION:0.0"
    tmux attach-session -t "$SESSION"
elif command -v gnome-terminal &> /dev/null; then
    # GNOME Terminal (no split panes, opens 4 tabs)
    gnome-terminal \
        --tab --title="website" -- bash -c "$CMD1; exec bash" \
        --tab --title="rag_server" -- bash -c "$CMD2; exec bash" \
        --tab --title="agent_panel" -- bash -c "$CMD3; exec bash" \
        --tab --title="router_server" -- bash -c "$CMD4; exec bash"
elif command -v konsole &> /dev/null; then
    # KDE Konsole
    konsole --new-tab -e bash -c "$CMD1; exec bash" &
    konsole --new-tab -e bash -c "$CMD2; exec bash" &
    konsole --new-tab -e bash -c "$CMD3; exec bash" &
    konsole --new-tab -e bash -c "$CMD4; exec bash" &
elif [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS Terminal.app or iTerm2
    if command -v osascript &> /dev/null; then
        osascript <<EOF
tell application "Terminal"
    activate
    do script "cd \"$WEBSITE_DIR\" && npm run dev"
    do script "cd \"$RAG_DIR\" && source \"$VENV_ACTIVATE\" && python rag_server.py"
    do script "cd \"$AGENT_PANEL_DIR\" && npm run dev"
    do script "cd \"$ROUTER_SERVER_DIR\" && cargo run"
end tell
EOF
    fi
else
    echo "No supported terminal emulator found."
    echo "Please install tmux, or run commands manually:"
    echo "  Pane 1: $CMD1"
    echo "  Pane 2: $CMD2"
    echo "  Pane 3: $CMD3"
    echo "  Pane 4: $CMD4"
    exit 1
fi

