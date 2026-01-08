cd "$(dirname "$0")/../PRIVATE/copper-ai-private/pcb_agent"
source .venv/Scripts/activate
python rag_server.py
exec bash
