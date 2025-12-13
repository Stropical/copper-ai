# KiCad Schematic Agent

Python agent that wraps the Ollama API for use with KiCad's schematic editor.

## Overview

This agent acts as an intermediary between the C++ KiCad code and Ollama, providing:
- HTTP API that matches Ollama's interface
- **Real-time continuous streaming** - forwards streaming responses from Ollama immediately as they arrive
- Non-streaming completion support
- Health checks and error handling
- CORS support for web-based clients

## Installation

### Using a virtual environment (recommended)

```bash
# Create virtual environment
python3 -m venv venv

# Activate virtual environment
source venv/bin/activate  # On macOS/Linux
# or
venv\Scripts\activate  # On Windows

# Install dependencies
pip install flask flask-cors requests urllib3
```

### Alternative: Using pipx

```bash
pipx install flask flask-cors requests urllib3
```

## Configuration

The agent can be configured via environment variables:

- `OLLAMA_BASE_URL`: URL of the Ollama server (default: `http://localhost:11434`)
- `AGENT_PORT`: Port for the agent server (default: `5000`)
- `AGENT_HOST`: Host for the agent server (default: `127.0.0.1`)
- `LOG_LEVEL`: Logging level - DEBUG, INFO, WARNING, ERROR (default: `INFO`)

### Logging

The agent logs all requests and errors in detail:

- **Request logging**: Every incoming request from KiCad is logged with:
  - Client IP address
  - User-Agent (shows it's from KiCad/curl)
  - Request method and path
  - Content-Type and Content-Length headers

- **Error logging**: All errors are logged with:
  - Full stack traces
  - Client information
  - Detailed error messages
  - HTTP status codes

- **Streaming logging**: Streaming requests log:
  - Connection status
  - Chunk counts
  - Any errors during streaming

To enable more verbose logging:
```bash
LOG_LEVEL=DEBUG python main.py
```

## Running

### With virtual environment

```bash
# Activate virtual environment first
source venv/bin/activate  # On macOS/Linux

# Run the agent
python main.py
```

### With custom configuration

```bash
source venv/bin/activate
OLLAMA_BASE_URL=http://192.168.177.144:11434 AGENT_PORT=5000 python main.py
```

The agent will start on `http://127.0.0.1:5000` by default, which matches the default URL in the C++ code.

## Testing the Connection

You can test if the agent is working correctly:

```bash
source venv/bin/activate
python test_connection.py
```

This will test:
- Health endpoint
- Tags endpoint (availability check)
- Generate endpoint (simple request)

## Troubleshooting

If you get "Failed to communicate with Python agent" error in KiCad:

1. **Check if the agent is running:**
   ```bash
   curl http://127.0.0.1:5000/health
   ```

2. **Check the agent logs** - The agent logs all requests and responses. Look for:
   - Connection errors
   - Timeout errors
   - Ollama availability issues

3. **Verify Ollama is accessible:**
   ```bash
   curl http://192.168.177.144:11434/api/tags
   ```
   (Replace with your Ollama URL if different)

4. **Check firewall/network** - Ensure port 5000 is accessible from KiCad

5. **Enable verbose curl logging in KiCad:**
   ```bash
   export KICAD_CURL_VERBOSE=1
   ```
   Then run KiCad to see detailed curl connection logs

## API Endpoints

- `POST /api/generate` - Generate completions (supports streaming via `stream: true`)
  - When `stream: true`, responses are forwarded in real-time as newline-delimited JSON (NDJSON)
  - Each line is sent immediately as it arrives from Ollama for continuous streaming
- `GET /api/tags` - List available Ollama models
- `GET /health` - Health check endpoint
- `GET /` - Service information

## Architecture

```
KiCad C++ Code → Python Agent (HTTP) → Ollama (HTTP)
```

The C++ code communicates with this Python agent via HTTP, and the Python agent forwards requests to Ollama. This separation allows for:
- **Real-time streaming** - continuous HTTP streaming from Ollama through the agent to KiCad
- Easier debugging and logging
- Future enhancements (caching, rate limiting, etc.)
- Better error handling and retries

### Streaming Behavior

The agent is optimized for real-time continuous streaming:
- Uses Flask's generator-based responses for immediate data transmission
- Forwards each line from Ollama as soon as it arrives (no buffering)
- Maintains persistent HTTP connections for long-running streams
- Handles connection errors gracefully during streaming


