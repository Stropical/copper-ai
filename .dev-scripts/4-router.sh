cd "$(dirname "$0")/../PRIVATE/copper-ai-private/router_server"
# Add cargo to PATH (standard Windows rustup location)
export PATH="$HOME/.cargo/bin:$USERPROFILE/.cargo/bin:/c/Users/$USER/.cargo/bin:$PATH"
cargo run
exec bash
