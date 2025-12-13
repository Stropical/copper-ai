# KiCad RAG Agent

Advanced RAG (Retrieval-Augmented Generation) agent with vector database for KiCad's schematic editor.

## Overview

This is a **true RAG agent** that provides:
- **Vector Database** - ChromaDB for knowledge storage and semantic search
- **Memory System** - Conversation history and long-term memory storage
- **RAG Capabilities** - Automatic context retrieval and prompt augmentation
- **Agent Orchestration** - Planning, tool use, and intelligent response generation
- **Real-time Streaming** - Continuous streaming responses from Ollama
- **Knowledge Management API** - Add, search, and manage knowledge base
- HTTP API compatible with Ollama interface

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
pip install -e .
# Or install directly:
pip install flask flask-cors requests urllib3 chromadb sentence-transformers tiktoken numpy
```

### Alternative: Using pipx

```bash
pipx install flask flask-cors requests urllib3 chromadb sentence-transformers tiktoken numpy
```

## Configuration

The agent can be configured via environment variables:

- `OLLAMA_BASE_URL`: URL of the Ollama server (default: `http://localhost:11434`)
- `AGENT_PORT`: Port for the agent server (default: `5001` - changed from 5000 to avoid macOS AirPlay conflict)
- `AGENT_HOST`: Host for the agent server (default: `127.0.0.1`)
- `AGENT_DATA_DIR`: Directory for storing vector DB and memory (default: `./agent_data`)
- `EMBEDDING_MODEL`: Ollama embedding model name (default: `nomic-embed-text`)
- `CHUNK_SIZE`: Text chunk size for documents (default: `500`)
- `CHUNK_OVERLAP`: Overlap between chunks (default: `50`)
- `TOP_K_RETRIEVAL`: Number of results to retrieve (default: `5`)
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
OLLAMA_BASE_URL=http://192.168.177.144:11434 AGENT_PORT=5001 python main.py
```

The agent will start on `http://127.0.0.1:5001` by default, which matches the default URL in the C++ code.

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
   curl http://127.0.0.1:5001/health
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

### Core Endpoints

- `POST /api/generate` - Generate completions with RAG (supports streaming via `stream: true`)
  - Automatically augments prompts with relevant knowledge from vector database
  - Maintains conversation context across requests (use `session_id` in request or `X-Session-ID` header)
  - When `stream: true`, responses are forwarded in real-time as newline-delimited JSON (NDJSON)
  - Request body can include `session_id` for conversation continuity

- `GET /api/tags` - List available Ollama models
- `GET /health` - Health check endpoint with knowledge base stats
- `GET /` - Service information

### Knowledge Management Endpoints

- `POST /api/knowledge/add` - Add knowledge to the vector database
  ```json
  {
    "content": "KiCad uses .kicad_sch files for schematics...",
    "source": "documentation",
    "metadata": {"category": "file_formats"}
  }
  ```

- `POST /api/knowledge/search` - Search the knowledge base
  ```json
  {
    "query": "How do I place components?",
    "top_k": 5
  }
  ```

- `DELETE /api/knowledge/delete/<document_id>` - Delete a document from knowledge base

- `GET /api/knowledge/stats` - Get knowledge base statistics

### Memory Endpoints

- `GET /api/memory/conversation/<session_id>` - Get conversation history
  - Query params: `limit` (default: 50)

## Architecture

```
KiCad C++ Code → RAG Agent (HTTP) → Ollama (HTTP)
                      ↓
              Vector Database (ChromaDB)
                      ↓
              Memory Database (SQLite)
```

### Components

1. **Vector Database (ChromaDB)**: Stores document embeddings for semantic search
2. **Embedding Service**: Uses Ollama's embedding API to generate vector embeddings
3. **Text Chunker**: Splits documents into chunks with overlap for better retrieval
4. **Memory Manager**: SQLite database for conversation history and long-term memory
5. **RAG Retriever**: Retrieves relevant context from knowledge base and augments prompts
6. **Agent Orchestrator**: Plans actions, manages tool use, and orchestrates agent behavior

### How RAG Works

1. **User Query** → Agent receives prompt from KiCad
2. **Planning** → Agent determines if context retrieval or tool use is needed
3. **Retrieval** → Relevant knowledge is retrieved from vector database using semantic search
4. **Augmentation** → Prompt is augmented with retrieved context and conversation history
5. **Generation** → Enhanced prompt is sent to Ollama
6. **Storage** → Response and conversation are stored in memory

### Features

- **Automatic Context Retrieval**: Queries automatically trigger knowledge base search
- **Conversation Memory**: Maintains context across multiple requests using session IDs
- **Tool Detection**: Automatically detects when tool calls are needed
- **Knowledge Learning**: Can store important information learned during conversations

### Streaming Behavior

The agent is optimized for real-time continuous streaming:
- Uses Flask's generator-based responses for immediate data transmission
- Forwards each line from Ollama as soon as it arrives (no buffering)
- Maintains persistent HTTP connections for long-running streams
- Handles connection errors gracefully during streaming

## Usage Examples

### Adding Knowledge to the Vector Database

```bash
curl -X POST http://127.0.0.1:5001/api/knowledge/add \
  -H "Content-Type: application/json" \
  -d '{
    "content": "KiCad schematics use .kicad_sch file format. Components are placed using the Place Component tool. Each component has a reference designator like R1, C1, U1, etc.",
    "source": "user_manual",
    "metadata": {"category": "basics", "topic": "file_formats"}
  }'
```

### Searching the Knowledge Base

```bash
curl -X POST http://127.0.0.1:5001/api/knowledge/search \
  -H "Content-Type: application/json" \
  -d '{
    "query": "How do I place components in KiCad?",
    "top_k": 3
  }'
```

### Using RAG in Generate Requests

The `/api/generate` endpoint automatically uses RAG. Include a `session_id` for conversation continuity:

```bash
curl -X POST http://127.0.0.1:5001/api/generate \
  -H "Content-Type: application/json" \
  -d '{
    "model": "llama3",
    "prompt": "How do I create a new schematic?",
    "session_id": "my-session-123",
    "stream": false
  }'
```

The agent will:
1. Search the knowledge base for relevant information
2. Retrieve conversation history for the session
3. Augment the prompt with this context
4. Generate a response using the enhanced prompt
5. Store the conversation in memory

### Getting Conversation History

```bash
curl http://127.0.0.1:5001/api/memory/conversation/my-session-123?limit=10
```

### Checking Knowledge Base Statistics

```bash
curl http://127.0.0.1:5001/api/knowledge/stats
```


