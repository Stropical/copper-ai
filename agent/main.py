#!/usr/bin/env python3
"""
RAG Agent with vector database for KiCad schematic agent.
This agent provides:
- Vector database for knowledge storage and retrieval
- Memory/conversation storage
- RAG (Retrieval-Augmented Generation) capabilities
- True agent system with planning and tool orchestration
"""

import json
import os
import logging
import sqlite3
import hashlib
import time
import uuid
from typing import Optional, List, Dict, Any, Tuple
from datetime import datetime
from pathlib import Path
from dataclasses import dataclass, asdict
from flask import Flask, request, Response, jsonify
from flask_cors import CORS
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
import chromadb
from chromadb.config import Settings
import tiktoken

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
logging.getLogger("werkzeug").setLevel(logging.WARNING)
logging.getLogger("chromadb").setLevel(logging.WARNING)

app = Flask(__name__)
CORS(app)

# Configuration
OLLAMA_BASE_URL = os.getenv("OLLAMA_BASE_URL", "http://192.168.177.144:11434")
AGENT_PORT = int(os.getenv("AGENT_PORT", "5001"))  # Changed from 5000 to avoid AirPlay conflict
AGENT_HOST = os.getenv("AGENT_HOST", "127.0.0.1")
DATA_DIR = Path(os.getenv("AGENT_DATA_DIR", "./agent_data"))
EMBEDDING_MODEL = os.getenv("EMBEDDING_MODEL", "nomic-embed-text")  # Ollama embedding model
CHUNK_SIZE = int(os.getenv("CHUNK_SIZE", "500"))
CHUNK_OVERLAP = int(os.getenv("CHUNK_OVERLAP", "50"))
TOP_K_RETRIEVAL = int(os.getenv("TOP_K_RETRIEVAL", "5"))
SUPPORTED_TOOL_NAMES = {"schematic.place_component", "schematic.move_component", "mock.selection_inspector"}

THINK_TAG_START = "<think>"
THINK_TAG_END = "</think>"
THINKING_DIRECTIVE_TEXT = (
    "REASONING VISIBILITY DIRECTIVE\n"
    "------------------------------\n"
    "Before emitting any TOOL lines, TASKS section, or COMMANDS section, briefly stream your internal reasoning "
    "inside a single <think>...</think> block.\n"
    "Only reasoning belongs in this block. After </think>, immediately output the strict TOOL/TASKS/COMMANDS format "
    "described elsewhere.\n"
    "Never include multiple <think> sections, never nest them, and never leave the block unclosed.\n"
)


def ensure_thinking_directive(system_prompt: Optional[str]) -> str:
    """Append the reasoning visibility directive to the system prompt exactly once."""
    base_prompt = system_prompt or ""

    if THINKING_DIRECTIVE_TEXT in base_prompt:
        return base_prompt

    if not base_prompt:
        return THINKING_DIRECTIVE_TEXT

    separator = "\n\n" if not base_prompt.endswith("\n") else "\n"
    return f"{base_prompt}{separator}{THINKING_DIRECTIVE_TEXT}"

# Ensure data directory exists
DATA_DIR.mkdir(parents=True, exist_ok=True)
VECTOR_DB_PATH = DATA_DIR / "chroma_db"
MEMORY_DB_PATH = DATA_DIR / "memory.db"

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


@dataclass
class Document:
    """Represents a document in the knowledge base."""
    id: str
    content: str
    metadata: Dict[str, Any]
    timestamp: float


@dataclass
class ConversationMessage:
    """Represents a message in a conversation."""
    id: str
    session_id: str
    role: str  # "user", "assistant", "system"
    content: str
    timestamp: float
    metadata: Dict[str, Any]


class TextChunker:
    """Handles text chunking for documents."""
    
    def __init__(self, chunk_size: int = CHUNK_SIZE, chunk_overlap: int = CHUNK_OVERLAP):
        self.chunk_size = chunk_size
        self.chunk_overlap = chunk_overlap
        try:
            self.encoding = tiktoken.get_encoding("cl100k_base")
        except Exception:
            logger.warning("tiktoken encoding not available, using character-based chunking")
            self.encoding = None
    
    def chunk_text(self, text: str) -> List[str]:
        """Split text into chunks with overlap."""
        if self.encoding:
            return self._chunk_with_tiktoken(text)
        else:
            return self._chunk_simple(text)
    
    def _chunk_with_tiktoken(self, text: str) -> List[str]:
        """Chunk using tiktoken tokenizer."""
        tokens = self.encoding.encode(text)
        chunks = []
        start = 0
        
        while start < len(tokens):
            end = start + self.chunk_size
            chunk_tokens = tokens[start:end]
            chunk_text = self.encoding.decode(chunk_tokens)
            chunks.append(chunk_text)
            
            if end >= len(tokens):
                break
            start = end - self.chunk_overlap
        
        return chunks
    
    def _chunk_simple(self, text: str) -> List[str]:
        """Simple character-based chunking."""
        chunks = []
        start = 0
        
        while start < len(text):
            end = start + self.chunk_size
            chunk = text[start:end]
            chunks.append(chunk)
            
            if end >= len(text):
                break
            start = end - self.chunk_overlap
        
        return chunks


class EmbeddingService:
    """Handles embedding generation using Ollama."""
    
    def __init__(self, ollama_url: str, model: str = EMBEDDING_MODEL):
        self.ollama_url = ollama_url
        self.model = model
        self.session = requests.Session()
    
    def get_embedding(self, text: str) -> Optional[List[float]]:
        """Get embedding for a single text."""
        max_retries = 2
        for attempt in range(max_retries):
            try:
                # Ollama uses /api/embed endpoint (not /api/embeddings)
                # Note: First request may take longer as model loads
                timeout = 60 if attempt == 0 else 30  # Longer timeout for first attempt
                response = self.session.post(
                    f"{self.ollama_url}/api/embeddings",
                    json={"model": self.model, "input": text},
                    timeout=timeout
                )
                response.raise_for_status()
                data = response.json()
                # Ollama /api/embed returns embeddings as a list of lists: {"embeddings": [[...]]}
                if "embeddings" in data and len(data["embeddings"]) > 0:
                    embedding = data["embeddings"][0]
                    if embedding and len(embedding) > 0:
                        return embedding
                # Fallback for other formats
                elif "embedding" in data:
                    embedding = data.get("embedding")
                    if embedding and len(embedding) > 0:
                        return embedding
                elif "data" in data and len(data["data"]) > 0:
                    embedding = data["data"][0].get("embedding")
                    if embedding and len(embedding) > 0:
                        return embedding
                    
                logger.warning(f"No embedding returned from Ollama for model {self.model}")
                return None
            except requests.exceptions.HTTPError as e:
                if e.response.status_code == 404:
                    logger.warning(f"Embedding model '{self.model}' not found. Install it with: ollama pull {self.model}")
                    logger.warning("RAG retrieval will be disabled until embedding model is installed")
                    return None
                elif e.response.status_code == 500:
                    # Server error - might be temporary, retry once
                    if attempt < max_retries - 1:
                        logger.debug(f"Ollama server error (500), retrying... (attempt {attempt + 1}/{max_retries})")
                        import time
                        time.sleep(0.5)  # Brief delay before retry
                        continue
                    else:
                        logger.warning(f"Ollama server error (500) after {max_retries} attempts. Response: {e.response.text[:200] if hasattr(e.response, 'text') else 'N/A'}")
                        return None
                else:
                    logger.error(f"HTTP error getting embedding: {e}")
                    if hasattr(e.response, 'text'):
                        logger.debug(f"Response body: {e.response.text[:500]}")
                    return None
            except requests.exceptions.Timeout:
                if attempt < max_retries - 1:
                    logger.debug(f"Embedding request timeout, retrying... (attempt {attempt + 1}/{max_retries})")
                    continue
                else:
                    logger.warning(f"Embedding request timed out after {max_retries} attempts")
                    return None
            except Exception as e:
                logger.debug(f"Error getting embedding: {e}")
                return None
        
        return None
    
    def get_embeddings(self, texts: List[str]) -> List[Optional[List[float]]]:
        """Get embeddings for multiple texts."""
        embeddings = []
        for text in texts:
            embedding = self.get_embedding(text)
            embeddings.append(embedding)
        return embeddings


class VectorDatabase:
    """Manages vector database for knowledge storage."""
    
    def __init__(self, db_path: Path, embedding_service: EmbeddingService):
        self.db_path = db_path
        self.embedding_service = embedding_service
        self.client = chromadb.PersistentClient(
            path=str(db_path),
            settings=Settings(anonymized_telemetry=False)
        )
        self.collection = self.client.get_or_create_collection(
            name="knowledge_base",
            metadata={"description": "KiCad schematic agent knowledge base"}
        )
        logger.info(f"Vector database initialized at {db_path}")
    
    def add_document(self, content: str, metadata: Dict[str, Any]) -> str:
        """Add a document to the knowledge base."""
        doc_id = str(uuid.uuid4())
        chunks = TextChunker().chunk_text(content)
        
        # Generate embeddings for chunks
        embeddings = []
        valid_chunks = []
        valid_ids = []
        
        for i, chunk in enumerate(chunks):
            embedding = self.embedding_service.get_embedding(chunk)
            if embedding:
                embeddings.append(embedding)
                valid_chunks.append(chunk)
                chunk_id = f"{doc_id}_chunk_{i}"
                valid_ids.append(chunk_id)
        
        if not valid_chunks:
            logger.warning(f"No valid embeddings generated for document {doc_id}")
            return doc_id
        
        # Add to collection
        chunk_metadata = [
            {**metadata, "chunk_index": i, "document_id": doc_id, "total_chunks": len(chunks)}
            for i in range(len(valid_chunks))
        ]
        
        self.collection.add(
            ids=valid_ids,
            embeddings=embeddings,
            documents=valid_chunks,
            metadatas=chunk_metadata
        )
        
        logger.info(f"Added document {doc_id} with {len(valid_chunks)} chunks")
        return doc_id
    
    def search(self, query: str, top_k: int = TOP_K_RETRIEVAL, filter_metadata: Optional[Dict] = None) -> List[Dict[str, Any]]:
        """Search the knowledge base."""
        query_embedding = self.embedding_service.get_embedding(query)
        if not query_embedding:
            logger.warning("Failed to generate query embedding")
            return []
        
        # Build query
        query_kwargs = {
            "query_embeddings": [query_embedding],
            "n_results": top_k
        }
        
        if filter_metadata:
            query_kwargs["where"] = filter_metadata
        
        results = self.collection.query(**query_kwargs)
        
        # Format results
        formatted_results = []
        if results["ids"] and len(results["ids"][0]) > 0:
            for i in range(len(results["ids"][0])):
                formatted_results.append({
                    "id": results["ids"][0][i],
                    "content": results["documents"][0][i],
                    "metadata": results["metadatas"][0][i],
                    "distance": results["distances"][0][i] if "distances" in results else None
                })
        
        return formatted_results
    
    def delete_document(self, doc_id: str) -> bool:
        """Delete a document and all its chunks."""
        try:
            # Get all chunks for this document
            results = self.collection.get(where={"document_id": doc_id})
            if results["ids"]:
                self.collection.delete(ids=results["ids"])
                logger.info(f"Deleted document {doc_id} with {len(results['ids'])} chunks")
                return True
            return False
        except Exception as e:
            logger.error(f"Error deleting document {doc_id}: {e}")
            return False
    
    def get_stats(self) -> Dict[str, Any]:
        """Get statistics about the knowledge base."""
        count = self.collection.count()
        return {
            "total_chunks": count,
            "collection_name": self.collection.name
        }


class MemoryManager:
    """Manages conversation memory and long-term storage."""
    
    def __init__(self, db_path: Path):
        self.db_path = db_path
        self._init_db()
    
    def _init_db(self):
        """Initialize the memory database."""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        # Conversations table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS conversations (
                id TEXT PRIMARY KEY,
                session_id TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                timestamp REAL NOT NULL,
                metadata TEXT
            )
        """)
        
        # Sessions table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS sessions (
                id TEXT PRIMARY KEY,
                created_at REAL NOT NULL,
                last_activity REAL NOT NULL,
                metadata TEXT
            )
        """)
        
        # Knowledge entries table (for tracking what was learned)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS knowledge_entries (
                id TEXT PRIMARY KEY,
                content TEXT NOT NULL,
                source TEXT,
                timestamp REAL NOT NULL,
                metadata TEXT
            )
        """)
        
        conn.commit()
        conn.close()
        logger.info(f"Memory database initialized at {self.db_path}")
    
    def add_message(self, session_id: str, role: str, content: str, metadata: Optional[Dict] = None) -> str:
        """Add a message to a conversation."""
        message_id = str(uuid.uuid4())
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        cursor.execute("""
            INSERT INTO conversations (id, session_id, role, content, timestamp, metadata)
            VALUES (?, ?, ?, ?, ?, ?)
        """, (message_id, session_id, role, content, time.time(), json.dumps(metadata or {})))
        
        # Update session activity
        cursor.execute("""
            INSERT OR REPLACE INTO sessions (id, created_at, last_activity, metadata)
            VALUES (?, COALESCE((SELECT created_at FROM sessions WHERE id = ?), ?), ?, ?)
        """, (session_id, session_id, time.time(), time.time(), json.dumps({})))
        
        conn.commit()
        conn.close()
        return message_id
    
    def get_conversation(self, session_id: str, limit: int = 50) -> List[ConversationMessage]:
        """Get conversation history for a session."""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        cursor.execute("""
            SELECT id, session_id, role, content, timestamp, metadata
            FROM conversations
            WHERE session_id = ?
            ORDER BY timestamp ASC
            LIMIT ?
        """, (session_id, limit))
        
        messages = []
        for row in cursor.fetchall():
            messages.append(ConversationMessage(
                id=row[0],
                session_id=row[1],
                role=row[2],
                content=row[3],
                timestamp=row[4],
                metadata=json.loads(row[5] or "{}")
            ))
        
        conn.close()
        return messages
    
    def add_knowledge(self, content: str, source: str, metadata: Optional[Dict] = None) -> str:
        """Record that knowledge was learned/stored."""
        entry_id = str(uuid.uuid4())
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        cursor.execute("""
            INSERT INTO knowledge_entries (id, content, source, timestamp, metadata)
            VALUES (?, ?, ?, ?, ?)
        """, (entry_id, content, source, time.time(), json.dumps(metadata or {})))
        
        conn.commit()
        conn.close()
        return entry_id


class RAGRetriever:
    """Handles RAG retrieval and context augmentation."""
    
    def __init__(self, vector_db: VectorDatabase, memory_manager: MemoryManager):
        self.vector_db = vector_db
        self.memory_manager = memory_manager
    
    def retrieve_context(self, query: str, session_id: Optional[str] = None, top_k: int = TOP_K_RETRIEVAL) -> str:
        """Retrieve relevant context for a query."""
        # Search vector database
        try:
            results = self.vector_db.search(query, top_k=top_k)
            
            if not results:
                return ""
            
            # Build context string
            context_parts = []
            context_parts.append("Relevant knowledge from knowledge base:")
            for i, result in enumerate(results, 1):
                content = result["content"]
                metadata = result.get("metadata", {})
                source = metadata.get("source", "unknown")
                context_parts.append(f"\n[{i}] Source: {source}\n{content}")
            
            return "\n".join(context_parts)
        except Exception as e:
            logger.debug(f"Error retrieving context (RAG disabled): {e}")
            return ""  # Return empty string to continue without RAG
    
    def augment_prompt(self, prompt: str, session_id: Optional[str] = None, include_memory: bool = True) -> str:
        """Augment a prompt with retrieved context and memory."""
        # Retrieve relevant context (may be empty if embeddings unavailable)
        context = self.retrieve_context(prompt, session_id)
        
        # Get conversation history if requested
        memory_context = ""
        if include_memory and session_id:
            try:
                messages = self.memory_manager.get_conversation(session_id, limit=10)
                if messages:
                    memory_context = "\n\nRecent conversation history:\n"
                    for msg in messages[-5:]:  # Last 5 messages
                        memory_context += f"{msg.role}: {msg.content}\n"
            except Exception as e:
                logger.debug(f"Error retrieving conversation history: {e}")
        
        # Combine
        augmented_parts = []
        if context:
            augmented_parts.append(context)
        if memory_context:
            augmented_parts.append(memory_context)
        if augmented_parts:
            augmented_parts.append(f"\n\nUser query: {prompt}")
            return "\n".join(augmented_parts)
        
        # If no augmentation, return original prompt
        return prompt


class AgentOrchestrator:
    """Orchestrates agent behavior with planning and tool use."""
    
    def __init__(self, ollama_url: str, rag_retriever: RAGRetriever, memory_manager: MemoryManager):
        self.ollama_url = ollama_url
        self.rag_retriever = rag_retriever
        self.memory_manager = memory_manager
        self.session = requests.Session()
    
    def plan_action(self, user_query: str, session_id: str, model: str) -> Dict[str, Any]:
        """Plan the next action based on user query."""
        # Simple planning: determine if we need to search, use tools, or just respond
        query_lower = user_query.lower()
        
        plan = {
            "action": "respond",  # "respond", "search", "tool_use"
            "needs_context": True,
            "tools": []
        }
        
        # Detect if tool use is needed
        if "place" in query_lower and "component" in query_lower:
            plan["action"] = "tool_use"
            plan["tools"] = ["schematic.place_component"]
        elif "search" in query_lower or "find" in query_lower or "what" in query_lower:
            plan["action"] = "search"
            plan["needs_context"] = True
        
        return plan
    
    def execute_plan(self, plan: Dict[str, Any], user_query: str, session_id: str, model: str, 
                    original_prompt: str, system_prompt: str) -> str:
        """Execute the planned action."""
        if plan["action"] == "search" or plan["needs_context"]:
            # Augment with RAG
            augmented_prompt = self.rag_retriever.augment_prompt(user_query, session_id)
        else:
            augmented_prompt = user_query
        
        # Store user message
        self.memory_manager.add_message(session_id, "user", user_query)
        
        return augmented_prompt
    
    def process_response(self, response: str, session_id: str, user_query: str):
        """Process and store the agent's response."""
        # Store assistant response
        self.memory_manager.add_message(session_id, "assistant", response)
        
        # Check if response contains learnable information
        if len(response) > 100 and not response.startswith("TOOL"):
            # Could extract and store important information here
            pass


# Initialize services
logger.info("Initializing RAG Agent services...")
embedding_service = EmbeddingService(OLLAMA_BASE_URL, EMBEDDING_MODEL)
vector_db = VectorDatabase(VECTOR_DB_PATH, embedding_service)
memory_manager = MemoryManager(MEMORY_DB_PATH)
rag_retriever = RAGRetriever(vector_db, memory_manager)
agent_orchestrator = AgentOrchestrator(OLLAMA_BASE_URL, rag_retriever, memory_manager)

# Check if embedding model is available
test_embedding = embedding_service.get_embedding("test")
if test_embedding is None:
    logger.warning(f"⚠ Embedding model '{EMBEDDING_MODEL}' is not available")
    logger.warning(f"  Install it with: ollama pull {EMBEDDING_MODEL}")
    logger.warning("  RAG retrieval will be disabled until the model is installed")
    logger.warning("  The agent will still work, but without knowledge base retrieval")
else:
    logger.info(f"✓ Embedding model '{EMBEDDING_MODEL}' is available")

logger.info("RAG Agent services initialized")


def validate_tool_call(line: str):
    """Validate and parse a tool call from the LLM response."""
    line = line.strip()
    
    if not line.startswith("TOOL "):
        return (False, None, None)
    
    rest = line[5:].strip()
    if not rest:
        logger.warning("Tool call missing tool name and payload")
        return (False, None, None)
    
    space_idx = rest.find(" ")
    if space_idx == -1:
        logger.warning(f"Tool call missing JSON payload: {line}")
        return (False, None, None)
    
    tool_name = rest[:space_idx].strip()
    json_str = rest[space_idx:].strip()
    
    if not tool_name or not json_str:
        logger.warning(f"Tool call '{tool_name}' has empty fields")
        return (False, tool_name, None)
    
    normalized_tool = tool_name.lower()

    if normalized_tool not in SUPPORTED_TOOL_NAMES:
        logger.warning(f"Tool call '{tool_name}' is not supported. Supported tools: {sorted(SUPPORTED_TOOL_NAMES)}")
        return (False, tool_name, None)

    try:
        payload = json.loads(json_str)
        if not isinstance(payload, dict):
            logger.warning(f"Tool call '{tool_name}' payload is not a JSON object")
            return (False, tool_name, None)
        
        if normalized_tool == "schematic.place_component":
            required_fields = ["symbol", "x", "y"]
            missing = [field for field in required_fields if field not in payload]
            if missing:
                logger.warning(f"Tool call '{tool_name}' missing required fields: {missing}")
                return (False, tool_name, payload)
            
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
        logger.warning(f"Tool call '{tool_name}' has invalid JSON: {e}")
        return (False, tool_name, None)
    except Exception as e:
        logger.error(f"Error validating tool call: {e}", exc_info=True)
        return (False, tool_name, None)


def sanitize_tool_lines(text: str) -> str:
    """
    Remove unsupported or invalid TOOL lines from a response chunk.

    Ollama streams arbitrary fragments. We conservatively split on line breaks and only pass
    through TOOL lines that map to supported tools with valid JSON payloads.
    """
    if "TOOL " not in text:
        return text

    cleaned_segments: List[str] = []

    # Keep newline characters via splitlines(keepends=True) so formatting stays stable.
    for segment in text.splitlines(keepends=True):
        stripped = segment.strip()

        if stripped.startswith("TOOL "):
            is_valid, tool_name, payload = validate_tool_call(stripped)

            if not is_valid:
                logger.warning(
                    "Dropping unsupported/invalid tool call from response: %s...",
                    stripped[:120],
                )
                continue
            else:
                logger.info(f"Valid tool call detected: {tool_name}")

        cleaned_segments.append(segment)

    return "".join(cleaned_segments)


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


# Request logging middleware
@app.before_request
def log_request_info():
    """Log all incoming requests."""
    logger.debug("=" * 60)
    logger.debug(f"Request: {request.method} {request.path}")
    logger.debug(f"  Client IP: {request.remote_addr}")

@app.after_request
def log_response_info(response):
    """Log response details."""
    logger.debug(f"Response: {response.status_code} for {request.method} {request.path}")
    logger.debug("=" * 60)
    return response


@app.route("/api/tags", methods=["GET"])
def tags():
    """Proxy to Ollama's /api/tags endpoint."""
    try:
        response = session.get(f"{OLLAMA_BASE_URL}/api/tags", timeout=5)
        return Response(
            response.content,
            status=response.status_code,
            headers={"Content-Type": "application/json"},
        )
    except Exception as e:
        logger.error(f"Error in /api/tags: {e}")
        return jsonify({"error": str(e)}), 503


@app.route("/api/generate", methods=["POST"])
def generate():
    """Enhanced generate endpoint with RAG capabilities."""
    client_ip = request.remote_addr
    
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "No JSON data provided"}), 400

        model = data.get("model", "unknown")
        original_prompt = data.get("prompt", "")
        system_prompt = ensure_thinking_directive(data.get("system", ""))
        stream = data.get("stream", False)
        
        # Get or create session ID
        session_id = data.get("session_id") or request.headers.get("X-Session-ID") or str(uuid.uuid4())
        
        logger.info("=" * 60)
        logger.info(f"Received POST /api/generate request")
        logger.info(f"  Model: {model}")
        logger.info(f"  Session ID: {session_id}")
        logger.info(f"  Streaming: {stream}")
        
        # Agent orchestration: plan and execute
        plan = agent_orchestrator.plan_action(original_prompt, session_id, model)
        logger.info(f"  Agent plan: {plan}")
        
        # Execute plan and augment prompt
        augmented_prompt = agent_orchestrator.execute_plan(
            plan, original_prompt, session_id, model, original_prompt, system_prompt
        )
        
        # Build request to Ollama
        ollama_data = {
            "model": model,
            "prompt": augmented_prompt,
            "stream": stream
        }
        if system_prompt:
            ollama_data["system"] = system_prompt
        
        ollama_url = f"{OLLAMA_BASE_URL}/api/generate"
        
        if stream:
            def generate_stream():
                stream_session = None
                ollama_response = None
                full_response_text = ""
                thinking_open = False
                try:
                    stream_session = requests.Session()
                    ollama_response = stream_session.post(
                        ollama_url,
                        json=ollama_data,
                        stream=True,
                        timeout=None,
                    )
                    ollama_response.raise_for_status()
                    
                    for line in ollama_response.iter_lines(decode_unicode=True, chunk_size=None):
                        if line:
                            if isinstance(line, bytes):
                                line = line.decode('utf-8', errors='replace')
                            elif not isinstance(line, str):
                                line = str(line)
                            
                            line = line.strip()
                            if line:
                                try:
                                    chunk_json = json.loads(line)

                                    # Wrap thinking chunks in <think> blocks for the UI bubbles
                                    if "thinking" in chunk_json:
                                        thinking_text = chunk_json.get("thinking", "")
                                        sanitized_thinking = sanitize_tool_lines(thinking_text)
                                        if sanitized_thinking:
                                            if not thinking_open:
                                                sanitized_thinking = f"{THINK_TAG_START}{sanitized_thinking}"
                                                thinking_open = True
                                            chunk_json["thinking"] = sanitized_thinking
                                        else:
                                            chunk_json.pop("thinking", None)

                                    # Sanitize response text but only send closing tags to the client copy
                                    if "response" in chunk_json:
                                        response_text = chunk_json.get("response", "")
                                        sanitized_response = sanitize_tool_lines(response_text)
                                        client_response = sanitized_response
                                        if client_response and thinking_open:
                                            client_response = f"{THINK_TAG_END}{client_response}"
                                            thinking_open = False

                                        chunk_json["response"] = client_response

                                        if sanitized_response:
                                            full_response_text += sanitized_response

                                    if chunk_json.get("done", False) and thinking_open:
                                        if chunk_json.get("thinking"):
                                            chunk_json["thinking"] = f"{chunk_json['thinking']}{THINK_TAG_END}"
                                        else:
                                            existing_response = chunk_json.get("response") or ""
                                            chunk_json["response"] = f"{THINK_TAG_END}{existing_response}"
                                        thinking_open = False

                                    line = json.dumps(chunk_json)

                                    if chunk_json.get("done", False):
                                        # Process and store response (without think tags)
                                        agent_orchestrator.process_response(
                                            full_response_text, session_id, original_prompt
                                        )
                                except (json.JSONDecodeError, KeyError):
                                    pass
                                
                                yield line + "\n"
                    
                except requests.exceptions.ChunkedEncodingError as e:
                    # Connection closed by Ollama - this is normal at end of stream
                    logger.info(f"Stream ended (chunked encoding closed)")
                    # Don't yield error, just end the stream gracefully
                    pass
                except Exception as e:
                    logger.error(f"Error during streaming: {e}", exc_info=True)
                    error_json = json.dumps({"error": str(e)})
                    yield error_json + "\n"
                finally:
                    if ollama_response:
                        try:
                            ollama_response.close()
                        except Exception:
                            pass
                    if stream_session:
                        try:
                            stream_session.close()
                        except Exception:
                            pass
            
            response = Response(
                generate_stream(),
                mimetype="application/json",
                headers={
                    "Content-Type": "application/json",
                    "Cache-Control": "no-cache, no-store, must-revalidate",
                    "X-Session-ID": session_id,
                },
            )
            response.headers.pop("Content-Length", None)
            return response
        else:
            # Non-streaming
            ollama_response = session.post(ollama_url, json=ollama_data, timeout=300)
            ollama_response.raise_for_status()
            response_data = ollama_response.json()
            response_text = response_data.get("response", "")
            sanitized_response = sanitize_tool_lines(response_text)

            thinking_text = response_data.get("thinking", "")
            sanitized_thinking = sanitize_tool_lines(thinking_text) if thinking_text else ""

            client_response_parts: List[str] = []
            if sanitized_thinking:
                client_response_parts.append(f"{THINK_TAG_START}{sanitized_thinking}{THINK_TAG_END}")
            client_response_parts.append(sanitized_response)

            response_data["response"] = "".join(client_response_parts)
            
            # Process and store response
            agent_orchestrator.process_response(sanitized_response, session_id, original_prompt)
            
            # Add session ID to response
            response_data["session_id"] = session_id
            
            return Response(
                json.dumps(response_data),
                status=ollama_response.status_code,
                headers={"Content-Type": "application/json", "X-Session-ID": session_id},
            )
    
    except Exception as e:
        logger.error(f"Error in /api/generate: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/api/knowledge/add", methods=["POST"])
def add_knowledge():
    """Add knowledge to the vector database."""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "No JSON data provided"}), 400
        
        content = data.get("content", "")
        if not content:
            return jsonify({"error": "Content is required"}), 400
        
        metadata = data.get("metadata", {})
        source = data.get("source", "api")
        metadata["source"] = source
        
        doc_id = vector_db.add_document(content, metadata)
        memory_manager.add_knowledge(content, source, metadata)
        
        return jsonify({
            "success": True,
            "document_id": doc_id,
            "message": "Knowledge added successfully"
        }), 200
    
    except Exception as e:
        logger.error(f"Error adding knowledge: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/api/knowledge/search", methods=["POST"])
def search_knowledge():
    """Search the knowledge base."""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "No JSON data provided"}), 400
        
        query = data.get("query", "")
        if not query:
            return jsonify({"error": "Query is required"}), 400
        
        top_k = data.get("top_k", TOP_K_RETRIEVAL)
        results = vector_db.search(query, top_k=top_k)
        
        return jsonify({
            "success": True,
            "results": results,
            "count": len(results)
        }), 200
    
    except Exception as e:
        logger.error(f"Error searching knowledge: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/api/knowledge/delete/<doc_id>", methods=["DELETE"])
def delete_knowledge(doc_id):
    """Delete a document from the knowledge base."""
    try:
        success = vector_db.delete_document(doc_id)
        if success:
            return jsonify({"success": True, "message": f"Document {doc_id} deleted"}), 200
        else:
            return jsonify({"success": False, "message": f"Document {doc_id} not found"}), 404
    except Exception as e:
        logger.error(f"Error deleting knowledge: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/api/knowledge/stats", methods=["GET"])
def knowledge_stats():
    """Get statistics about the knowledge base."""
    try:
        stats = vector_db.get_stats()
        return jsonify({"success": True, "stats": stats}), 200
    except Exception as e:
        logger.error(f"Error getting stats: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/api/memory/conversation/<session_id>", methods=["GET"])
def get_conversation(session_id):
    """Get conversation history for a session."""
    try:
        limit = request.args.get("limit", 50, type=int)
        messages = memory_manager.get_conversation(session_id, limit=limit)
        return jsonify({
            "success": True,
            "session_id": session_id,
            "messages": [asdict(msg) for msg in messages],
            "count": len(messages)
        }), 200
    except Exception as e:
        logger.error(f"Error getting conversation: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/health", methods=["GET"])
def health():
    """Health check endpoint."""
    ollama_available = check_ollama_available()
    kb_stats = vector_db.get_stats()
    return jsonify({
        "status": "ok",
        "ollama_available": ollama_available,
        "ollama_url": OLLAMA_BASE_URL,
        "knowledge_base": kb_stats,
        "vector_db_path": str(VECTOR_DB_PATH),
        "memory_db_path": str(MEMORY_DB_PATH),
    }), 200 if ollama_available else 503


@app.route("/", methods=["GET"])
def root():
    """Root endpoint with basic info."""
    return jsonify({
        "service": "KiCad RAG Agent",
        "version": "0.2.0",
        "ollama_url": OLLAMA_BASE_URL,
        "endpoints": {
            "/api/generate": "POST - Generate completions with RAG (supports streaming)",
            "/api/tags": "GET - List available models",
            "/api/knowledge/add": "POST - Add knowledge to vector database",
            "/api/knowledge/search": "POST - Search knowledge base",
            "/api/knowledge/delete/<id>": "DELETE - Delete document from knowledge base",
            "/api/knowledge/stats": "GET - Get knowledge base statistics",
            "/api/memory/conversation/<session_id>": "GET - Get conversation history",
            "/health": "GET - Health check",
        },
    })


if __name__ == "__main__":
    logger.info("=" * 60)
    logger.info("Starting KiCad RAG Agent")
    logger.info(f"Agent URL: http://{AGENT_HOST}:{AGENT_PORT}")
    logger.info(f"Ollama URL: {OLLAMA_BASE_URL}")
    logger.info(f"Vector DB: {VECTOR_DB_PATH}")
    logger.info(f"Memory DB: {MEMORY_DB_PATH}")
    logger.info("=" * 60)
    
    if check_ollama_available():
        logger.info("Agent ready to accept requests")
    else:
        logger.warning("⚠ Warning: Ollama server is not available")
        logger.warning(f"  Make sure Ollama is running at {OLLAMA_BASE_URL}")
    
    logger.info(f"Listening on http://{AGENT_HOST}:{AGENT_PORT}")
    logger.info("Press Ctrl+C to stop")
    logger.info("=" * 60)
    
    app.run(host=AGENT_HOST, port=AGENT_PORT, threaded=True, debug=False)
