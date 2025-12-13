#!/usr/bin/env python3
"""
Python agent that wraps Ollama API for KiCad schematic agent.
This agent acts as an intermediary between the C++ code and Ollama.
"""

import json
import os
import logging
from typing import Optional
from flask import Flask, request, Response, jsonify
from flask_cors import CORS
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# Configure logging
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL, logging.INFO),
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# Log curl/requests library errors at DEBUG level
logging.getLogger("urllib3").setLevel(logging.WARNING)
logging.getLogger("requests").setLevel(logging.WARNING)

# Disable Werkzeug request logging at INFO level (too verbose)
logging.getLogger("werkzeug").setLevel(logging.WARNING)

app = Flask(__name__)
CORS(app)  # Enable CORS for all routes

# Configure Flask for streaming - disable buffering
app.config['SEND_FILE_MAX_AGE_DEFAULT'] = 0

# Ensure Flask uses proper chunked encoding for generator responses
# Flask automatically handles this, but we want to make sure it's enabled
app.config['USE_X_SENDFILE'] = False

# Request logging middleware
@app.before_request
def log_request_info():
    """Log all incoming requests with curl/KiCad details."""
    logger.debug("=" * 60)
    logger.debug(f"Request: {request.method} {request.path}")
    logger.debug(f"  Client IP: {request.remote_addr}")
    logger.debug(f"  User-Agent: {request.headers.get('User-Agent', 'Unknown')}")
    logger.debug(f"  Content-Type: {request.headers.get('Content-Type', 'None')}")
    logger.debug(f"  Content-Length: {request.headers.get('Content-Length', 'Unknown')}")
    
    # Log curl-specific headers if present
    curl_headers = {k: v for k, v in request.headers.items() if 'curl' in k.lower() or 'ki' in k.lower()}
    if curl_headers:
        logger.debug(f"  Curl/KiCad headers: {curl_headers}")

@app.after_request
def log_response_info(response):
    """Log response details."""
    logger.debug(f"Response: {response.status_code} for {request.method} {request.path}")
    logger.debug(f"  Content-Length: {response.headers.get('Content-Length', 'Unknown')}")
    logger.debug("=" * 60)
    return response

# Enable Flask request logging
logging.getLogger('werkzeug').setLevel(logging.INFO)

# Configuration
OLLAMA_BASE_URL = os.getenv("OLLAMA_BASE_URL", "http://192.168.177.144:11434")
AGENT_PORT = int(os.getenv("AGENT_PORT", "5000"))
AGENT_HOST = os.getenv("AGENT_HOST", "127.0.0.1")

# Create a session with retry strategy
session = requests.Session()
retry_strategy = Retry(
    total=3,
    backoff_factor=0.3,
    status_forcelist=[429, 500, 502, 503, 504],
)
adapter = HTTPAdapter(max_retries=retry_strategy)
session.mount("http://", adapter)
session.mount("https://", adapter)


def validate_tool_call(line: str):
    """
    Validate and parse a tool call from the LLM response.
    
    Expected format: TOOL <tool_name> <json_object>
    
    Returns:
        tuple: (is_valid: bool, tool_name: Optional[str], json_payload: Optional[dict])
               Returns (False, None, None) if invalid
    """
    line = line.strip()
    
    # Check if line starts with TOOL
    if not line.startswith("TOOL "):
        return (False, None, None)
    
    # Extract the rest after "TOOL "
    rest = line[5:].strip()
    if not rest:
        logger.warning("Tool call missing tool name and payload")
        return (False, None, None)
    
    # Find the first space to separate tool name from JSON
    space_idx = rest.find(" ")
    if space_idx == -1:
        logger.warning(f"Tool call missing JSON payload: {line}")
        return (False, None, None)
    
    tool_name = rest[:space_idx].strip()
    json_str = rest[space_idx:].strip()
    
    if not tool_name:
        logger.warning("Tool call has empty tool name")
        return (False, None, None)
    
    if not json_str:
        logger.warning(f"Tool call '{tool_name}' has empty JSON payload")
        return (False, None, None)
    
    # Validate JSON
    try:
        payload = json.loads(json_str)
        if not isinstance(payload, dict):
            logger.warning(f"Tool call '{tool_name}' payload is not a JSON object: {json_str}")
            return (False, tool_name, None)
        
        # Validate specific tool schemas
        if tool_name == "schematic.place_component":
            required_fields = ["symbol", "x", "y"]
            missing = [field for field in required_fields if field not in payload]
            if missing:
                logger.warning(f"Tool call '{tool_name}' missing required fields: {missing}")
                return (False, tool_name, payload)
            
            # Validate types
            if not isinstance(payload.get("symbol"), str):
                logger.warning(f"Tool call '{tool_name}' field 'symbol' must be a string")
                return (False, tool_name, payload)
            
            if not isinstance(payload.get("x"), (int, float)):
                logger.warning(f"Tool call '{tool_name}' field 'x' must be a number")
                return (False, tool_name, payload)
            
            if not isinstance(payload.get("y"), (int, float)):
                logger.warning(f"Tool call '{tool_name}' field 'y' must be a number")
                return (False, tool_name, payload)
        
        logger.debug(f"Valid tool call: {tool_name} with payload: {json.dumps(payload)}")
        return (True, tool_name, payload)
        
    except json.JSONDecodeError as e:
        logger.warning(f"Tool call '{tool_name}' has invalid JSON: {json_str[:100]}... Error: {e}")
        return (False, tool_name, None)
    except Exception as e:
        logger.error(f"Error validating tool call: {e}", exc_info=True)
        return (False, tool_name, None)


def filter_and_validate_response_chunk(chunk: str) -> str:
    """
    Filter and validate response chunks, logging tool call issues.
    This doesn't modify the chunk (since we're streaming), but logs validation results.
    """
    # Check if this chunk contains a tool call
    if "TOOL " in chunk:
        # Try to extract tool call lines
        lines = chunk.split("\n")
        for line in lines:
            if line.strip().startswith("TOOL "):
                is_valid, tool_name, payload = validate_tool_call(line)
                if not is_valid:
                    logger.warning(f"Invalid tool call detected in stream: {line[:100]}...")
                else:
                    logger.info(f"Valid tool call detected: {tool_name}")
    
    return chunk


def check_ollama_available() -> bool:
    """Check if Ollama server is available."""
    try:
        logger.info(f"Checking Ollama availability at {OLLAMA_BASE_URL}/api/tags")
        response = session.get(f"{OLLAMA_BASE_URL}/api/tags", timeout=2)
        available = response.status_code == 200
        if available:
            logger.info("✓ Ollama server is available")
        else:
            logger.warning(f"✗ Ollama server returned status {response.status_code}")
        return available
    except Exception as e:
        logger.error(f"✗ Ollama server check failed: {e}")
        return False


@app.route("/api/tags", methods=["GET"])
def tags():
    """Proxy to Ollama's /api/tags endpoint for availability checks."""
    client_ip = request.remote_addr
    user_agent = request.headers.get("User-Agent", "Unknown")
    logger.info(f"Received GET /api/tags request from {client_ip}")
    logger.debug(f"User-Agent: {user_agent}")
    try:
        logger.debug(f"Forwarding to Ollama: {OLLAMA_BASE_URL}/api/tags")
        response = session.get(f"{OLLAMA_BASE_URL}/api/tags", timeout=5)
        logger.info(f"Ollama /api/tags returned status {response.status_code} (content length: {len(response.content)} bytes)")
        return Response(
            response.content,
            status=response.status_code,
            headers={"Content-Type": "application/json"},
        )
    except requests.exceptions.Timeout:
        logger.error(f"Timeout connecting to Ollama at {OLLAMA_BASE_URL}/api/tags")
        logger.error(f"  Request from: {client_ip}, User-Agent: {user_agent}")
        return jsonify({"error": "Timeout connecting to Ollama"}), 503
    except requests.exceptions.ConnectionError as e:
        logger.error(f"Connection error to Ollama at {OLLAMA_BASE_URL}/api/tags: {e}")
        logger.error(f"  Request from: {client_ip}, User-Agent: {user_agent}")
        logger.error(f"  Error details: {str(e)}")
        return jsonify({"error": f"Cannot connect to Ollama: {str(e)}"}), 503
    except Exception as e:
        logger.error(f"Error in /api/tags: {e}", exc_info=True)
        logger.error(f"  Request from: {client_ip}, User-Agent: {user_agent}")
        import traceback
        logger.error(f"  Traceback: {traceback.format_exc()}")
        return jsonify({"error": str(e)}), 503


@app.route("/api/generate", methods=["POST"])
def generate():
    """Proxy to Ollama's /api/generate endpoint."""
    client_ip = request.remote_addr
    user_agent = request.headers.get("User-Agent", "Unknown")
    
    logger.info("=" * 60)
    logger.info(f"Received POST /api/generate request from {client_ip}")
    logger.info(f"User-Agent: {user_agent}")
    
    try:
        data = request.get_json()
        if not data:
            logger.error("No JSON data provided in request")
            return jsonify({"error": "No JSON data provided"}), 400

        model = data.get("model", "unknown")
        prompt_length = len(data.get("prompt", ""))
        stream = data.get("stream", False)
        has_system = "system" in data
        
        logger.info(f"Request details:")
        logger.info(f"  Model: {model}")
        logger.info(f"  Prompt length: {prompt_length} characters")
        logger.info(f"  Streaming: {stream}")
        logger.info(f"  Has system prompt: {has_system}")
        logger.info(f"  Client IP: {client_ip}")
        logger.info(f"  User-Agent: {user_agent}")
        if prompt_length < 200:
            logger.info(f"  Prompt preview: {data.get('prompt', '')[:100]}...")

        # Forward request to Ollama
        ollama_url = f"{OLLAMA_BASE_URL}/api/generate"
        logger.info(f"Forwarding request to Ollama: {ollama_url}")
        
        if stream:
            # Handle streaming response - forward in real-time continuously
            logger.info("Starting streaming response...")
            line_count = 0
            
            def generate_stream():
                nonlocal line_count
                stream_session = None
                ollama_response = None
                try:
                    # Use a fresh session for streaming to avoid connection pooling issues
                    stream_session = requests.Session()
                    
                    logger.info(f"Connecting to Ollama for streaming: {ollama_url}")
                    # Disable response buffering for real-time streaming
                    ollama_response = stream_session.post(
                        ollama_url,
                        json=data,
                        stream=True,
                        timeout=None,  # No timeout for streaming
                    )
                    logger.info(f"Ollama connection established, status: {ollama_response.status_code}")
                    ollama_response.raise_for_status()
                    
                    # Stream each line immediately as it arrives from Ollama
                    # Use iter_lines to get complete lines, which is more efficient
                    # and ensures we don't split JSON objects
                    # Note: decode_unicode=True should return strings, but we handle bytes as fallback
                    for line in ollama_response.iter_lines(decode_unicode=True, chunk_size=None):
                        if line:
                            # Ensure line is a string (not bytes)
                            # Sometimes iter_lines returns bytes even with decode_unicode=True
                            if isinstance(line, bytes):
                                line = line.decode('utf-8', errors='replace')
                            elif not isinstance(line, str):
                                # Convert any other type to string
                                line = str(line)
                            
                            # Ollama returns newline-delimited JSON (NDJSON)
                            # Each line is a complete JSON object
                            line = line.strip()
                            if line:
                                line_count += 1
                                if line_count == 1:
                                    logger.info("First chunk received from Ollama, streaming started")
                                elif line_count % 10 == 0:
                                    logger.debug(f"Streamed {line_count} chunks so far...")
                                
                                # Validate tool calls in the response (for logging/debugging)
                                # Note: We don't modify the stream, just log validation results
                                try:
                                    # Try to parse as JSON to check if it contains response text
                                    chunk_json = json.loads(line)
                                    
                                    # Check for done flag - this indicates stream completion
                                    if chunk_json.get("done", False):
                                        logger.info(f"Received 'done' flag from Ollama (chunk {line_count})")
                                    
                                    if "response" in chunk_json:
                                        response_text = chunk_json.get("response", "")
                                        if response_text and "TOOL " in response_text:
                                            # Check for tool calls in the response text
                                            filter_and_validate_response_chunk(response_text)
                                except (json.JSONDecodeError, KeyError):
                                    # Not a response chunk or not JSON, skip validation
                                    pass
                                
                                # Yield immediately - Flask will send this right away
                                # The C++ client expects newline-delimited JSON
                                # Flask automatically handles chunked encoding for generators
                                # Each yield becomes a chunk, Flask adds proper chunk headers
                                line_str = str(line) + "\n"
                                yield line_str
                    
                    logger.info(f"Streaming complete. Total chunks: {line_count}")
                        
                except requests.exceptions.ChunkedEncodingError as e:
                    # Connection closed by Ollama - this is normal at end of stream
                    logger.info(f"Stream ended (chunked encoding closed). Total chunks: {line_count}")
                    pass
                except requests.exceptions.Timeout as e:
                    logger.error(f"Timeout during streaming: {e}")
                    logger.error(f"  Client: {client_ip}, User-Agent: {user_agent}")
                    error_json = json.dumps({"error": f"Timeout during streaming: {str(e)}"})
                    yield error_json + "\n"
                except requests.exceptions.ConnectionError as e:
                    # Connection error - send error as JSON line
                    logger.error(f"Connection error during streaming: {e}", exc_info=True)
                    logger.error(f"  Client: {client_ip}, User-Agent: {user_agent}")
                    logger.error(f"  Ollama URL: {OLLAMA_BASE_URL}")
                    error_json = json.dumps({"error": f"Connection error: {str(e)}"})
                    yield error_json + "\n"
                except requests.exceptions.HTTPError as e:
                    logger.error(f"HTTP error during streaming: {e}")
                    logger.error(f"  Client: {client_ip}, User-Agent: {user_agent}")
                    if hasattr(e.response, 'status_code'):
                        logger.error(f"  HTTP Status: {e.response.status_code}")
                    error_json = json.dumps({"error": f"HTTP error: {str(e)}"})
                    yield error_json + "\n"
                except requests.exceptions.RequestException as e:
                    logger.error(f"Request error during streaming: {e}", exc_info=True)
                    logger.error(f"  Client: {client_ip}, User-Agent: {user_agent}")
                    error_json = json.dumps({"error": f"Request error: {str(e)}"})
                    yield error_json + "\n"
                except Exception as e:
                    # Send error as JSON line
                    logger.error(f"Unexpected error during streaming: {e}", exc_info=True)
                    logger.error(f"  Client: {client_ip}, User-Agent: {user_agent}")
                    import traceback
                    logger.error(f"  Traceback: {traceback.format_exc()}")
                    error_json = json.dumps({"error": str(e)})
                    yield error_json + "\n"
                finally:
                    # Ensure connections are closed to prevent hanging
                    if ollama_response is not None:
                        try:
                            ollama_response.close()
                            logger.debug("Ollama response closed")
                        except Exception as e:
                            logger.debug(f"Error closing ollama response: {e}")
                    if stream_session is not None:
                        try:
                            stream_session.close()
                            logger.debug("Stream session closed")
                        except Exception as e:
                            logger.debug(f"Error closing stream session: {e}")
            
            # Return streaming response with headers optimized for real-time streaming
            # Flask automatically handles chunked encoding for generator responses
            # We must NOT set Transfer-Encoding manually - Flask/Werkzeug does this automatically
            # Setting it manually causes duplicate headers and curl parsing errors
            response = Response(
                generate_stream(),
                mimetype="application/json",
                headers={
                    "Content-Type": "application/json",
                    "Cache-Control": "no-cache, no-store, must-revalidate",
                    "Pragma": "no-cache",
                    "Expires": "0",
                    "Connection": "keep-alive",
                    "X-Accel-Buffering": "no",  # Disable nginx buffering if present
                },
            )
            # Ensure Flask doesn't add Content-Length (would break chunked encoding)
            response.headers.pop("Content-Length", None)
            return response
        else:
            # Handle non-streaming response
            logger.info("Handling non-streaming request")
            ollama_response = session.post(
                ollama_url,
                json=data,
                timeout=300,  # 5 minute timeout for non-streaming
            )
            logger.info(f"Non-streaming response received, status: {ollama_response.status_code}")
            ollama_response.raise_for_status()
            
            response_length = len(ollama_response.content)
            logger.info(f"Response length: {response_length} bytes")
            
            return Response(
                ollama_response.content,
                status=ollama_response.status_code,
                headers={"Content-Type": "application/json"},
            )
    
    except requests.exceptions.Timeout as e:
        logger.error(f"Timeout connecting to Ollama: {e}")
        logger.error(f"  Request was from: {client_ip}")
        logger.error(f"  User-Agent: {user_agent}")
        return jsonify({"error": f"Timeout connecting to Ollama: {str(e)}"}), 503
    except requests.exceptions.ConnectionError as e:
        logger.error(f"Connection error to Ollama: {e}")
        logger.error(f"  Request was from: {client_ip}")
        logger.error(f"  User-Agent: {user_agent}")
        logger.error(f"  Ollama URL: {OLLAMA_BASE_URL}")
        return jsonify({"error": f"Cannot connect to Ollama at {OLLAMA_BASE_URL}: {str(e)}"}), 503
    except requests.exceptions.HTTPError as e:
        logger.error(f"HTTP error from Ollama: {e}")
        logger.error(f"  Request was from: {client_ip}")
        logger.error(f"  User-Agent: {user_agent}")
        if hasattr(e.response, 'status_code'):
            logger.error(f"  HTTP Status: {e.response.status_code}")
        if hasattr(e.response, 'text'):
            logger.error(f"  Response: {e.response.text[:500]}")
        return jsonify({"error": f"HTTP error from Ollama: {str(e)}"}), 503
    except requests.exceptions.RequestException as e:
        logger.error(f"Request exception in /api/generate: {e}", exc_info=True)
        logger.error(f"  Request was from: {client_ip}")
        logger.error(f"  User-Agent: {user_agent}")
        return jsonify({"error": f"Failed to communicate with Ollama: {str(e)}"}), 503
    except Exception as e:
        logger.error(f"Unexpected error in /api/generate: {e}", exc_info=True)
        logger.error(f"  Request was from: {client_ip}")
        logger.error(f"  User-Agent: {user_agent}")
        import traceback
        logger.error(f"  Traceback: {traceback.format_exc()}")
        return jsonify({"error": f"Internal error: {str(e)}"}), 500


@app.route("/health", methods=["GET"])
def health():
    """Health check endpoint."""
    logger.debug("Health check requested")
    ollama_available = check_ollama_available()
    return jsonify({
        "status": "ok",
        "ollama_available": ollama_available,
        "ollama_url": OLLAMA_BASE_URL,
    }), 200 if ollama_available else 503


@app.route("/", methods=["GET"])
def root():
    """Root endpoint with basic info."""
    logger.debug("Root endpoint accessed")
    return jsonify({
        "service": "KiCad Schematic Agent",
        "version": "0.1.0",
        "ollama_url": OLLAMA_BASE_URL,
        "endpoints": {
            "/api/generate": "POST - Generate completions (supports streaming)",
            "/api/tags": "GET - List available models",
            "/health": "GET - Health check",
        },
    })


if __name__ == "__main__":
    logger.info("=" * 60)
    logger.info("Starting KiCad Schematic Agent")
    logger.info(f"Agent URL: http://{AGENT_HOST}:{AGENT_PORT}")
    logger.info(f"Ollama URL: {OLLAMA_BASE_URL}")
    logger.info("=" * 60)
    
    # Check Ollama availability at startup
    if check_ollama_available():
        logger.info("Agent ready to accept requests")
    else:
        logger.warning("⚠ Warning: Ollama server is not available")
        logger.warning(f"  Make sure Ollama is running at {OLLAMA_BASE_URL}")
        logger.warning("  The agent will still start but requests may fail")
    
    logger.info(f"Listening on http://{AGENT_HOST}:{AGENT_PORT}")
    logger.info("Press Ctrl+C to stop")
    logger.info("=" * 60)
    
    # Run with threading enabled for concurrent requests
    # Use threaded=True to handle multiple streaming connections
    app.run(host=AGENT_HOST, port=AGENT_PORT, threaded=True, debug=False)

