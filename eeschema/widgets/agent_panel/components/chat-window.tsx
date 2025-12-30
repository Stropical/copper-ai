"use client"

import * as React from "react"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Avatar, AvatarFallback } from "@/components/ui/avatar"
import { Separator } from "@/components/ui/separator"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select"
import { ToolCallComponent, ToolCall } from "@/components/tool-call"
import { Send, User, Bot, Sparkles, Check, X } from "lucide-react"
import { Streamdown } from "streamdown"

interface Message {
  id: string
  role: "user" | "assistant" | "thinking"
  content: string
  timestamp: Date
  toolCalls?: ToolCall[]
  todos?: { id: string; task: string; status: "pending" | "completed" | "cancelled" | "in_progress" }[]
  agent?: string
}

const MODELS = [
  { value: "google/gemma-3-27b-it:free", label: "Gemma 3 27B IT (Google)" },
  { value: "openai/gpt-oss-120b:free", label: "GPT-OSS 120B (OpenAI)" },
  { value: "qwen/qwen3-coder:free", label: "Qwen3 Coder (Qwen)"},
  { value: "meta-llama/llama-3.1-405b-instruct:free", label: "Llama 3.1 405B Instruct (Meta)"}
]

const AGENT_API_URL = process.env.NEXT_PUBLIC_AGENT_API_URL || "http://127.0.0.1:5001/api/generate"
const AGENT_BASE_URL = AGENT_API_URL.replace(/\/api\/generate\/?$/, "")

// Serial tool mode: execute one tool call at a time, then let the model continue.
const SERIAL_TOOL_MODE = false

export function ChatWindow() {
  const [messages, setMessages] = React.useState<Message[]>([
    {
      id: "1",
      role: "assistant",
      content: "Hello! I'm your AI assistant. How can I help you today?",
      timestamp: new Date(),
    },
  ])
  const [input, setInput] = React.useState("")
  const [isLoading, setIsLoading] = React.useState(false)
  const [queuedPrompt, setQueuedPrompt] = React.useState<string | null>(null)
  const [selectedModel, setSelectedModel] = React.useState("google/gemma-3-4b")
  const [sessionId] = React.useState(() => `session-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`)
  const [authRequired, setAuthRequired] = React.useState<{
    loginUrl?: string
    tokenPageUrl?: string
  } | null>(null)
  const messagesEndRef = React.useRef<HTMLDivElement>(null)
  const queuedPromptRef = React.useRef<string | null>(null)
  const queuedAutoPromptRef = React.useRef<string | null>(null)
  const rpcWaitersRef = React.useRef(new Map<number, { resolve: (v: any) => void; reject: (e: any) => void }>())
  const rpcIdRef = React.useRef(0)
  const abortControllerRef = React.useRef<AbortController | null>(null)

  const scrollToBottom = () => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" })
  }

  const stopStreaming = React.useCallback(() => {
    if (abortControllerRef.current) {
      try {
        abortControllerRef.current.abort()
      } catch {
        // ignore
      } finally {
        abortControllerRef.current = null
      }
    }
  }, [])

  React.useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        stopStreaming()
      }
    }
    window.addEventListener("keydown", onKeyDown)
    return () => window.removeEventListener("keydown", onKeyDown)
  }, [stopStreaming])

  // --- KiCad WebView RPC bridge (to fetch schematic context) ---
  const postToKiCad = (payload: string): boolean => {
    try {
      const w = window as any

      if (w.webkit?.messageHandlers?.kicad?.postMessage) {
        w.webkit.messageHandlers.kicad.postMessage(payload)
        return true
      }

      if (w.chrome?.webview?.postMessage) {
        w.chrome.webview.postMessage(payload)
        return true
      }

      if (w.external?.invoke && typeof w.external.invoke === "function") {
        w.external.invoke(payload)
        return true
      }
    } catch (e) {
      console.warn("Failed to post to KiCad bridge:", e)
    }

    return false
  }

  React.useEffect(() => {
    const w = window as any
    const existing = w.kiclient || {}
    const prevPost =
      typeof existing.postMessage === "function" ? existing.postMessage.bind(existing) : null

    existing.postMessage = (incoming: any) => {
      try {
        const payload = typeof incoming === "string" ? JSON.parse(incoming) : incoming
        if (!payload || typeof payload !== "object") return

        const responseTo = payload.response_to
        if (typeof responseTo === "number") {
          const waiter = rpcWaitersRef.current.get(responseTo)
          if (waiter) {
            rpcWaitersRef.current.delete(responseTo)
            waiter.resolve(payload)
          }
        }
      } catch (e) {
        // Ignore parse errors
      } finally {
        if (prevPost) prevPost(incoming)
      }
    }

    w.kiclient = existing
  }, [])

  const sendRpcCommand = async (command: string, parameters?: Record<string, any>) => {
    const messageId = ++rpcIdRef.current
    const payload = JSON.stringify({
      command,
      message_id: messageId,
      parameters: parameters || {},
    })

    return new Promise<any>((resolve, reject) => {
      rpcWaitersRef.current.set(messageId, { resolve, reject })

      const ok = postToKiCad(payload)
      if (!ok) {
        rpcWaitersRef.current.delete(messageId)
        reject(new Error("No KiCad bridge available"))
        return
      }

      // Timeout (don’t block sending forever)
      window.setTimeout(() => {
        if (rpcWaitersRef.current.has(messageId)) {
          rpcWaitersRef.current.delete(messageId)
          reject(new Error("KiCad RPC timeout"))
        }
      }, 5000)
    })
  }

  const tryGetSchematicContext = async (): Promise<string> => {
    try {
      // Keep full context available to the Python agent; we'll control log verbosity there.
      const resp = await sendRpcCommand("GET_SCHEMATIC_CONTEXT", { max_chars: 50000 })
      if (resp?.status === "OK" && typeof resp.data === "string" && resp.data.trim()) {
        return resp.data
      }
      return ""
    } catch (e) {
      return ""
    }
  }

  const runToolCallNow = async (
    messageId: string,
    toolCallId: string,
    toolName: string,
    payload: any
  ) => {
    // Mark running
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id !== messageId) return msg
        const updatedToolCalls = msg.toolCalls?.map((tc) =>
          tc.id === toolCallId ? { ...tc, status: "running" as const } : tc
        )
        return { ...msg, toolCalls: updatedToolCalls }
      })
    )

    try {
      const resp = await sendRpcCommand("RUN_TOOL", { tool_name: toolName, payload })
      if (!resp || typeof resp !== "object") {
        throw new Error("Invalid response from tool execution")
      }
      
      const ok = resp.status === "OK"
      const respData = resp.data
      const errorMessage = resp.error_message

      const isReadOnlyTool = toolName === "schematic.search_symbol" || toolName === "schematic.get_datasheet"

      // If this was a datasheet query, ingest it into pcb_agent's RAG DB automatically.
      let datasheetIngestNote: string | null = null
      if (ok && toolName === "schematic.get_datasheet" && typeof respData === "string" && respData.trim()) {
        try {
          const ingestResp = await fetch(`${AGENT_BASE_URL}/api/datasheet/ingest`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              kicad_datasheet: respData,
              session_id: sessionId,
              source: "kicad_tool_schematic.get_datasheet",
            }),
          }).then((r) => r.json())

          if (ingestResp?.success && ingestResp?.document_id) {
            datasheetIngestNote = `RAG: stored datasheet doc_id=${ingestResp.document_id} (ref=${ingestResp.reference || "?"})`
          } else {
            datasheetIngestNote = `RAG: ingest failed (${ingestResp?.error || "unknown error"})`
          }
        } catch (e: any) {
          datasheetIngestNote = `RAG: ingest failed (${e?.message || "network error"})`
        }
      }

      // After execution, keep it as "pending" until the user explicitly Accepts.
      setMessages((prev) =>
        prev.map((msg) => {
          if (msg.id !== messageId) return msg
          const updatedToolCalls = msg.toolCalls?.map((tc) =>
            tc.id === toolCallId
              ? {
                  ...tc,
                  // Bug fix: Do not block awaiting accept mid-run; auto-accept successful tools and allow undo at the end.
                  status: ok ? ("accepted" as const) : ("rejected" as const),
                  result: ok
                    ? (datasheetIngestNote ? `${respData || ""}\n\n${datasheetIngestNote}` : (typeof respData === "string" ? respData : "Applied"))
                    : (typeof errorMessage === "string" ? errorMessage : "Execution failed"),
                }
              : tc
          )
          
                          // Update todos: mark completed when tool succeeds
                          // Bug fix: Only mark the current in_progress todo as completed, not all matching ones
                          let updatedTodos = msg.todos
                          if (ok && msg.todos) {
                            let foundInProgress = false
                            updatedTodos = msg.todos.map((todo) => {
                              // Only complete the first in_progress todo (the current one)
                              if (!foundInProgress && todo.status === "in_progress") {
                                foundInProgress = true
                                return { ...todo, status: "completed" as const }
                              }
                              return todo
                            })
                          }
          
          return { ...msg, toolCalls: updatedToolCalls, todos: updatedTodos }
        })
      )

      // Agentic auto-continue: after tool calls, feed results back to the model.
      // Bug fix: Enable chaining for all successful tools, not just read-only ones
      // This allows the agent to continue to the next step after placement/wiring
      const shouldChain =
        ok &&
        typeof respData === "string" &&
        respData.trim()

      if (shouldChain && respData) {
        // Silently continue the agent chain without showing a user message
        const continuation = [
          "TOOL_RESULT",
          `tool_name: ${toolName}`,
          "data:",
          respData,
          "",
          "Continue the original task. Use this tool result and proceed to the next step.",
        ].join("\n")

        if (isLoading) {
          // Prefer continuing the agentic chain before any user-typed queued prompt.
          queuedAutoPromptRef.current = continuation
        } else {
          // Fire-and-forget to keep UI responsive - send silently (no user message)
          setTimeout(() => void sendPrompt(continuation, true), 0)
        }
      }
    } catch (e: any) {
      setMessages((prev) =>
        prev.map((msg) => {
          if (msg.id !== messageId) return msg
          const updatedToolCalls = msg.toolCalls?.map((tc) =>
            tc.id === toolCallId
              ? {
                  ...tc,
                  status: "rejected" as const,
                  result: e?.message || "Execution failed",
                }
              : tc
          )
          return { ...msg, toolCalls: updatedToolCalls }
        })
      )
    }
  }

  const handleToolCallAccept = async (messageId: string, toolCallId: string) => {
    // Mark as accepted in UI - the tool already executed and committed via commit.Push()
    // Accept is just a UI state change to indicate user approval
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id !== messageId) return msg
        const updatedToolCalls = msg.toolCalls?.map((tc) =>
          tc.id === toolCallId && tc.status === "pending" ? { ...tc, status: "accepted" as const } : tc
        )
        return { ...msg, toolCalls: updatedToolCalls }
      })
    )
    
    // Clear selection to ensure visibility
    try {
      await sendRpcCommand("CLEAR_SELECTION", {})
    } catch (e) {
      // Ignore errors - clearing selection is optional
    }
  }

  const handleToolCallUndo = (messageId: string, toolCallId: string) => {
    // Undo = revert the last applied KiCad operation (undo-stack semantics).
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id !== messageId) return msg
        const updatedToolCalls = msg.toolCalls?.map((tc) =>
          tc.id === toolCallId ? { ...tc, status: "running" as const } : tc
        )
        return { ...msg, toolCalls: updatedToolCalls }
      })
    )

    ;(async () => {
      try {
        const resp = await sendRpcCommand("UNDO", { count: 1 })
        void sendRpcCommand("CLEAR_SELECTION", {})
        const ok = resp?.status === "OK"

        setMessages((prev) =>
          prev.map((msg) => {
            if (msg.id !== messageId) return msg
            const updatedToolCalls = msg.toolCalls?.map((tc) =>
              tc.id === toolCallId
                ? {
                    ...tc,
                    status: ok ? ("rejected" as const) : ("accepted" as const),
                    result: ok ? "Undone" : (resp?.error_message || "Undo failed"),
                  }
                : tc
            )
            return { ...msg, toolCalls: updatedToolCalls }
          })
        )
      } catch (e: any) {
        void sendRpcCommand("CLEAR_SELECTION", {})
        setMessages((prev) =>
          prev.map((msg) => {
            if (msg.id !== messageId) return msg
            const updatedToolCalls = msg.toolCalls?.map((tc) =>
              tc.id === toolCallId
                ? {
                    ...tc,
                    status: "accepted" as const,
                    result: e?.message || "Undo failed",
                  }
                : tc
            )
            return { ...msg, toolCalls: updatedToolCalls }
          })
        )
      }
    })()
  }

  // Get the latest assistant message with tool calls
  const latestMessageWithToolCalls = React.useMemo(() => {
    return messages
      .filter((msg) => msg.role === "assistant" && msg.toolCalls && msg.toolCalls.length > 0)
      .slice(-1)[0]
  }, [messages])

  const hasPendingToolCalls = latestMessageWithToolCalls?.toolCalls?.some(
    (tc) => tc.status === "pending"
  ) ?? false

  const hasAcceptedToolCalls = latestMessageWithToolCalls?.toolCalls?.some(
    (tc) => tc.status === "accepted"
  ) ?? false

  const handleAcceptAll = () => {
    if (!latestMessageWithToolCalls) return
    void sendRpcCommand("CLEAR_SELECTION", {})
    // Tools already ran; Accept All just keeps them.
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id !== latestMessageWithToolCalls.id) return msg
        const updatedToolCalls = msg.toolCalls?.map((tc) =>
          tc.status === "pending" ? { ...tc, status: "accepted" as const } : tc
        )
        return { ...msg, toolCalls: updatedToolCalls }
      })
    )
  }

  const handleUndoAll = () => {
    if (!latestMessageWithToolCalls) return
    // Bug fix: Only count accepted tools for undo - pending ones haven't been committed to undo stack
    const countToUndo =
      latestMessageWithToolCalls.toolCalls?.filter((tc) => tc.status === "accepted").length ||
      0

    if (countToUndo <= 0) return

    // Mark as running
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id !== latestMessageWithToolCalls.id) return msg
        const updatedToolCalls = msg.toolCalls?.map((tc) =>
          tc.status === "accepted" || tc.status === "pending" ? { ...tc, status: "running" as const } : tc
        )
        return { ...msg, toolCalls: updatedToolCalls }
      })
    )

    ;(async () => {
      try {
        const resp = await sendRpcCommand("UNDO", { count: countToUndo })
        void sendRpcCommand("CLEAR_SELECTION", {})
        const ok = resp?.status === "OK"

        setMessages((prev) =>
          prev.map((msg) => {
            if (msg.id !== latestMessageWithToolCalls.id) return msg
            const updatedToolCalls = msg.toolCalls?.map((tc) =>
              tc.status === "running"
                ? {
                    ...tc,
                    status: ok ? ("rejected" as const) : ("accepted" as const),
                    result: ok ? "Undone" : (resp?.error_message || "Undo failed"),
                  }
                : tc
            )
            return { ...msg, toolCalls: updatedToolCalls }
          })
        )
      } catch {
        void sendRpcCommand("CLEAR_SELECTION", {})
        // If undo failed, leave them accepted.
        setMessages((prev) =>
          prev.map((msg) => {
            if (msg.id !== latestMessageWithToolCalls.id) return msg
            const updatedToolCalls = msg.toolCalls?.map((tc) =>
              tc.status === "running" ? { ...tc, status: "accepted" as const, result: "Undo failed" } : tc
            )
            return { ...msg, toolCalls: updatedToolCalls }
          })
        )
      }
    })()
  }

  // Parse tool calls from response text.
  // Supports:
  // - Lines like: "TOOL schematic.add_wire { ...json... }"
  // - Lines inside ```tool fenced blocks like: "schematic.add_wire { ...json... }"
  const parseToolCalls = (text: string): ToolCall[] => {
    const toolCalls: ToolCall[] = []

    const parseToolLine = (line: string) => {
      const trimmed = line.trim()
      if (!trimmed) return

      // Format A: TOOL <name> <json>
      if (trimmed.startsWith("TOOL ")) {
        const rest = trimmed.slice(5).trim()
        const spaceIdx = rest.indexOf(" ")
        if (spaceIdx === -1) return

        const toolName = rest.slice(0, spaceIdx).trim()
        const jsonStr = rest.slice(spaceIdx).trim()

        try {
          const args = JSON.parse(jsonStr)
          toolCalls.push({
            id: `tool-${Date.now()}-${toolCalls.length}`,
            name: toolName,
            arguments: args,
            status: "pending",
          })
        } catch {
          console.warn("Failed to parse tool call JSON:", jsonStr)
        }

        return
      }

      // Format B: <name> <json> (common inside ```tool fences)
      const match = trimmed.match(/^([a-zA-Z0-9_.:-]+)\s+(\{[\s\S]*\})$/)
      if (!match) return

      const toolName = match[1]
      const jsonStr = match[2]

      try {
        const args = JSON.parse(jsonStr)
        toolCalls.push({
          id: `tool-${Date.now()}-${toolCalls.length}`,
          name: toolName,
          arguments: args,
          status: "pending",
        })
      } catch {
        console.warn("Failed to parse tool call JSON:", jsonStr)
      }
    }

    const lines = text.split("\n")
    let inToolFence = false

    for (const line of lines) {
      const trimmed = line.trim()

      if (trimmed.startsWith("```")) {
        const fenceLang = trimmed.slice(3).trim().toLowerCase()
        if (!inToolFence && fenceLang === "tool") {
          inToolFence = true
          continue
        }

        if (inToolFence) {
          inToolFence = false
          continue
        }
      }

      if (inToolFence) {
        parseToolLine(line)
        continue
      }

      parseToolLine(line)
    }

    return toolCalls
  }

  // Remove tool calls, plan sections, and JSON plan objects from text to get clean content
  const removeToolCalls = (text: string): string => {
    const lines = text.split("\n")
    const kept: string[] = []
    let inToolFence = false
    let inPlanSection = false
    let inJsonBlock = false
    let jsonDepth = 0

    for (const line of lines) {
      const trimmed = line.trim()

      // Handle code fences
      if (trimmed.startsWith("```")) {
        const fenceLang = trimmed.slice(3).trim().toLowerCase()
        if (!inToolFence && fenceLang === "tool") {
          inToolFence = true
          continue
        }

        if (inToolFence) {
          inToolFence = false
          continue
        }
        
        // Skip other code fences (like json)
        if (fenceLang === "json" || fenceLang === "") {
          inJsonBlock = !inJsonBlock
          continue
        }
      }

      if (inToolFence || inJsonBlock) {
        continue
      }

      // Drop standalone tool lines
      if (trimmed.startsWith("TOOL ")) continue
      if (/^([a-zA-Z0-9_.:-]+)\s+\{[\s\S]*\}$/.test(trimmed)) continue
      
      // Drop "Plan:" headers and plan sections
      if (trimmed.match(/^Plan:\s*$/i)) {
        inPlanSection = true
        continue
      }
      
      // Stop plan section after a blank line or new section
      if (inPlanSection && (trimmed === "" || trimmed.match(/^(Manager|Specialist|TOOL|json)/i))) {
        if (trimmed !== "" && !trimmed.match(/^(Manager|Specialist)/i)) {
          inPlanSection = false
        } else if (trimmed.match(/^(TOOL|json)/i)) {
          inPlanSection = false
        }
      }
      
      if (inPlanSection) {
        continue
      }
      
      // Drop JSON plan objects
      if (trimmed.startsWith("{")) {
        try {
          const parsed = JSON.parse(trimmed)
          if (parsed.plan || parsed.todos) {
            continue
          }
        } catch {
          // Not JSON, keep the line
        }
      }

      kept.push(line)
    }

    return kept.join("\n")
  }

  // Split <think>...</think> blocks out of the streamed text.
  // Returns:
  // - visibleText: content excluding the full <think> blocks
  // - thinkingText: concatenated thinking content
  // - thinkingOpen: true if we saw an opening <think> without a closing </think> yet
  const splitThinking = (text: string): { visibleText: string; thinkingText: string; thinkingOpen: boolean } => {
    const OPEN = "<think>"
    const CLOSE = "</think>"

    let remaining = text || ""
    let visible = ""
    let thinking = ""
    let open = false

    while (remaining.length > 0) {
      const openIdx = remaining.indexOf(OPEN)
      if (openIdx === -1) {
        visible += remaining
        break
      }

      // Append visible chunk before <think>
      visible += remaining.slice(0, openIdx)
      remaining = remaining.slice(openIdx + OPEN.length)

      const closeIdx = remaining.indexOf(CLOSE)
      if (closeIdx === -1) {
        open = true
        thinking += remaining
        break
      }

      thinking += remaining.slice(0, closeIdx)
      remaining = remaining.slice(closeIdx + CLOSE.length)
    }

    // Some models/users may include stray tags; clean up lightly.
    visible = visible.replace(/<\/redacted_reasoning>/g, "")
    thinking = thinking.replace(/<\/redacted_reasoning>/g, "")

    return {
      visibleText: visible,
      thinkingText: thinking.trim(),
      thinkingOpen: open,
    }
  }

  const sendPrompt = async (promptText: string, silent: boolean = false) => {
    const trimmedPrompt = promptText.trim()
    if (!trimmedPrompt) return

    // Only add user message if not silent (silent = agent auto-continuation)
    if (!silent) {
      const userMessage: Message = {
        id: Date.now().toString(),
        role: "user",
        content: trimmedPrompt,
        timestamp: new Date(),
      }
      setMessages((prev) => [...prev, userMessage])
    }
    setIsLoading(true)

    try {
      setAuthRequired(null)
      const abortController = new AbortController()
      abortControllerRef.current = abortController

      // Include KiCad schematic context if available
      const schematicContext = await tryGetSchematicContext()
      const requestPrompt = schematicContext ? `${trimmedPrompt}\n\n${schematicContext}` : trimmedPrompt

      // Create thinking + assistant messages that will be updated as we stream
      const assistantMessageId = `assistant-${Date.now()}`
      const thinkingMessageId = `thinking-${assistantMessageId}`
      const managerMessageId = `manager-${assistantMessageId}`
      let accumulatedText = ""
      let lastVisibleText = ""

      const assistantMessage: Message = {
        id: assistantMessageId,
        role: "assistant",
        content: "",
        timestamp: new Date(),
        toolCalls: [],
        agent: "specialist",
      }
      const managerMessage: Message = {
        id: managerMessageId,
        role: "assistant",
        content: "",
        timestamp: new Date(),
        agent: "manager",
      }
      const thinkingMessage: Message = {
        id: thinkingMessageId,
        role: "thinking",
        content: "",
        timestamp: new Date(),
      }

      setMessages((prev) => [...prev, thinkingMessage, managerMessage, assistantMessage])

      const response = await fetch(AGENT_API_URL, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-Session-ID": sessionId,
        },
        signal: abortController.signal,
        body: JSON.stringify({
          model: selectedModel,
          prompt: requestPrompt,
          stream: true,
          session_id: sessionId,
        }),
      })

      if (!response.ok) {
        throw new Error(`API error: ${response.status} ${response.statusText}`)
      }

      const reader = response.body?.getReader()
      const decoder = new TextDecoder()

      if (!reader) {
        throw new Error("No response body")
      }

      while (true) {
        const { done, value } = await reader.read()
        if (done) break

        const chunk = decoder.decode(value, { stream: true })
        const lines = chunk.split("\n")

        for (const line of lines) {
          const trimmed = line.trim()
          if (!trimmed) continue

          try {
            const data = JSON.parse(trimmed)
            const responseText = data.response || ""
            const agentTag = typeof data.agent === "string" ? data.agent : null
            const targetMessageId = agentTag === "manager" ? managerMessageId : assistantMessageId

            // Parse plan/todos from the response
            if (data.plan && data.plan.todos && Array.isArray(data.plan.todos)) {
              setMessages((prev) =>
                prev.map((msg) =>
                  msg.id === assistantMessageId
                    ? {
                        ...msg,
                        todos: data.plan.todos.map((todo: any) => ({
                          id: todo.id || `todo-${Date.now()}-${Math.random()}`,
                          task: todo.task || todo.description || "",
                          status: (todo.status || "pending") as "pending" | "completed" | "cancelled" | "in_progress",
                        })),
                      }
                    : msg
                )
              )
            }

            // If the agent signals auth is required, show a login banner/button.
            if (data.auth_required) {
              setAuthRequired({
                loginUrl: data.login_url,
                tokenPageUrl: data.token_page_url,
              })
            }

            if (responseText) {
              // Update the agent label on the correct message bubble
              if (agentTag) {
                setMessages((prev) =>
                  prev.map((msg) => (msg.id === targetMessageId ? { ...msg, agent: agentTag } : msg))
                )
              }

              // Accumulate content for tool parsing only from the specialist/executor stream
              if (targetMessageId === assistantMessageId) {
                accumulatedText += responseText
              }

              // Update message with accumulated text (but don't show tool calls yet)
              const textToRender = targetMessageId === assistantMessageId ? accumulatedText : responseText
              const { visibleText, thinkingText, thinkingOpen } = splitThinking(textToRender)
              if (targetMessageId === assistantMessageId) {
                lastVisibleText = visibleText
              }
              const contentWithoutTools = removeToolCalls(visibleText).trim()

              setMessages((prev) =>
                prev.map((msg) =>
                  msg.id === targetMessageId
                    ? {
                        ...msg,
                        content: contentWithoutTools || "Processing...",
                      }
                    : msg
                )
              )

              // Update the thinking row (smaller/darker UI in render). If no thinking, keep hidden.
              if (thinkingText || thinkingOpen) {
                const displayThinking = thinkingOpen
                  ? `Thinking…\n${thinkingText}`.trim()
                  : thinkingText

                setMessages((prev) =>
                  prev.map((msg) =>
                    msg.id === thinkingMessageId
                      ? {
                          ...msg,
                          content: displayThinking,
                        }
                      : msg
                  )
                )
              } else {
                setMessages((prev) =>
                  prev.map((msg) =>
                    msg.id === thinkingMessageId
                      ? {
                          ...msg,
                          content: "",
                        }
                      : msg
                  )
                )
              }
            }

            if (data.done) {
              // Only parse tool calls from the visible (non-<think>) text.
              const toolCalls = parseToolCalls(lastVisibleText)

              if (toolCalls.length > 0) {
                console.info("Tool calls detected:", toolCalls)
                const toolCallsToShow = SERIAL_TOOL_MODE ? toolCalls.slice(0, 1) : toolCalls

                setMessages((prev) =>
                  prev.map((msg) =>
                    msg.id === assistantMessageId
                      ? {
                          ...msg,
                          toolCalls: toolCallsToShow,
                        }
                      : msg
                  )
                )

                // Run tool calls sequentially
                for (let i = 0; i < toolCallsToShow.length; i++) {
                  const firstTc = toolCallsToShow[i]
                  const isLast = i === toolCallsToShow.length - 1
                  
                  // Update todos: mark relevant todo as "in_progress"
                  setMessages((prev) =>
                    prev.map((msg) =>
                      msg.id === assistantMessageId
                        ? {
                            ...msg,
                            todos: msg.todos?.map((todo) => {
                              // If todo task mentions this tool or component, mark as in_progress
                              const toolNameLower = firstTc.name.toLowerCase()
                              const todoLower = todo.task.toLowerCase()
                              if (
                                todoLower.includes(toolNameLower) ||
                                todoLower.includes("place") ||
                                todoLower.includes("connect") ||
                                todoLower.includes("wire") ||
                                todoLower.includes("search")
                              ) {
                                return { ...todo, status: "in_progress" as const }
                              }
                              return todo
                            }),
                          }
                        : msg
                    )
                  )

                                  await runToolCallNow(assistantMessageId, firstTc.id, firstTc.name, firstTc.arguments)
                }

                if (toolCalls.length > 1 && SERIAL_TOOL_MODE) {
                  console.info(
                    "Serial tool mode enabled: remaining tool calls will be planned/executed after the model continues."
                  )
                }
              }

              // When finished, keep thinking text only if it exists; otherwise hide it.
              const { thinkingText } = splitThinking(accumulatedText)
              setMessages((prev) =>
                prev.map((msg) =>
                  msg.id === thinkingMessageId
                    ? {
                        ...msg,
                        content: thinkingText,
                      }
                    : msg
                )
              )
              break
            }
          } catch (e) {
            continue
          }
        }
      }
    } catch (error) {
      // Abort is a normal "stop" path
      if (error instanceof DOMException && error.name === "AbortError") {
        setMessages((prev) =>
          prev.map((msg) =>
            msg.role === "assistant" && msg.content === "" ? { ...msg, content: "Stopped." } : msg
          )
        )
        return
      }
      console.error("Error calling agent API:", error)
      setMessages((prev) => [
        ...prev,
        {
          id: `error-${Date.now()}`,
          role: "assistant",
          content: `Error: ${error instanceof Error ? error.message : "Failed to get response from agent"}`,
          timestamp: new Date(),
        },
      ])
    } finally {
      abortControllerRef.current = null
      setIsLoading(false)

      // Auto-send queued prompt (if you typed ahead while streaming)
      // Auto-continuation prompts are sent silently (no user message shown)
      const autoNext = queuedAutoPromptRef.current
      if (autoNext && autoNext.trim()) {
        queuedAutoPromptRef.current = null
        setTimeout(() => void sendPrompt(autoNext, true), 0)
      } else {
        const next = queuedPromptRef.current
        if (next && next.trim()) {
          queuedPromptRef.current = null
          setQueuedPrompt(null)
          // Fire-and-forget; avoid blocking UI - user prompts are NOT silent
          setTimeout(() => void sendPrompt(next, false), 0)
        }
      }
    }
  }

  const handleSend = async () => {
    const trimmed = input.trim()
    if (!trimmed) return

    // If we're already streaming, queue the next prompt.
    if (isLoading) {
      queuedPromptRef.current = trimmed
      setQueuedPrompt(trimmed)
      setInput("")
      return
    }

    setInput("")
    await sendPrompt(trimmed)
  }

  const handleKeyPress = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault()
      handleSend()
    }
  }

  React.useEffect(() => {
    scrollToBottom()
  }, [messages])

  return (
    <div className="flex h-screen flex-col bg-[#0F1115] text-[#E7E9EC]">
      {/* Header - Minimal */}
      <div className="border-b border-[#22272F] bg-[#0F1115] px-4 py-2">
        <div className="flex items-center gap-2">
          <Avatar className="h-6 w-6 border border-[#2C333D]">
            <AvatarFallback className="bg-[#C7773A] text-[#121417]">
              <Sparkles className="h-3 w-3" />
            </AvatarFallback>
          </Avatar>
          <h2 className="font-medium text-xs text-[#E7E9EC]">
            {isLoading ? (queuedPrompt ? "Thinking... (next queued)" : "Thinking...") : "AI Assistant"}
          </h2>
        </div>
      </div>

      {/* Messages */}
      <ScrollArea className="flex-1">
        <div className="space-y-4 px-4 py-4 overflow-x-hidden">
          {messages.map((message) => {
            // Handle thinking messages
            if (message.role === "thinking") {
              if (!message.content.trim()) {
                return null
              }

              return (
                <div key={message.id} className="flex gap-3 justify-start">
                  <Avatar className="h-6 w-6 shrink-0 border border-[#2C333D]">
                    <AvatarFallback className="bg-[#171B22] text-[#8B949E]">
                      <Bot className="h-3 w-3" />
                    </AvatarFallback>
                  </Avatar>
                  <div
                    className={`min-w-0 max-w-[80%] text-[11px] leading-snug text-[#7C8590] font-mono py-1 whitespace-pre-wrap break-words ${
                      isLoading ? "animate-pulse" : ""
                    }`}
                  >
                    {message.content}
                  </div>
                </div>
              )
            }

            return (
              <div key={message.id} className="space-y-4">
                <div
                  className={`flex gap-3 ${
                    message.role === "user" ? "justify-end" : "justify-start"
                  }`}
                >
                  {message.role === "assistant" && (
                    <Avatar className="h-6 w-6 shrink-0 border border-[#2C333D]">
                      <AvatarFallback className="bg-[#C7773A] text-[#121417]">
                        <Bot className="h-3 w-3" />
                      </AvatarFallback>
                    </Avatar>
                  )}
                  <div
                    className={`max-w-[80%] rounded-lg px-3 py-2 ${
                      message.role === "user"
                        ? "bg-[#C7773A] text-[#121417]"
                        : "bg-[#151A21] border border-[#2C333D] text-[#E7E9EC]"
                    }`}
                  >
                    {message.role === "assistant" && message.agent && message.content.trim() && (
                      <div className="text-[10px] uppercase tracking-wide text-[#9AA3AD] mb-1">
                        {message.agent === "manager"
                          ? "Manager"
                          : message.agent === "part_finder"
                          ? "Part Finder"
                          : message.agent === "schematic_designer"
                          ? "Schematic Designer"
                          : message.agent}
                      </div>
                    )}
                    <Streamdown
                      mode={
                        message.role === "assistant" &&
                        isLoading &&
                        messages[messages.length - 1]?.id === message.id
                          ? "streaming"
                          : "static"
                      }
                      className="text-[13px] leading-relaxed break-words [&_p]:my-1 [&_p:first-child]:mt-0 [&_p:last-child]:mb-0 [&_pre]:my-2 [&_pre]:overflow-x-auto [&_code]:break-words"
                      components={{
                        a: ({ href, children, ...props }) => (
                          <a
                            href={href}
                            target="_blank"
                            rel="noreferrer"
                            className="underline underline-offset-2"
                            {...props}
                          >
                            {children}
                          </a>
                        ),
                      }}
                    >
                      {message.content}
                    </Streamdown>
                  </div>
                  {message.role === "user" && (
                    <Avatar className="h-6 w-6 shrink-0 border border-[#2C333D]">
                      <AvatarFallback className="bg-[#171B22] text-[#E7E9EC]">
                        <User className="h-3 w-3" />
                      </AvatarFallback>
                    </Avatar>
                  )}
                </div>

                {/* Todos - Display plan */}
                {message.todos && message.todos.length > 0 && (
                  <div className="ml-9 space-y-1">
                    <div className="text-xs text-[#9AA3AD] font-medium mb-1">Plan:</div>
                    {message.todos.map((todo) => (
                      <div
                        key={todo.id}
                        className="flex items-center gap-2 text-xs text-[#9AA3AD]"
                      >
                        <div className="w-4 h-4 flex items-center justify-center shrink-0">
                          {todo.status === "completed" ? (
                            <Check className="h-3 w-3 text-[#C7773A]" />
                          ) : todo.status === "in_progress" ? (
                            <div className="w-2 h-2 rounded-full bg-[#C7773A] animate-pulse" />
                          ) : todo.status === "cancelled" ? (
                            <X className="h-3 w-3 text-[#9AA3AD]" />
                          ) : (
                            <div className="w-2 h-2 rounded-full border border-[#2C333D]" />
                          )}
                        </div>
                        <span
                          className={
                            todo.status === "completed"
                              ? "text-[#7C8590] line-through"
                              : todo.status === "in_progress"
                              ? "text-[#C7773A]"
                              : "text-[#9AA3AD]"
                          }
                        >
                          {todo.task}
                        </span>
                      </div>
                    ))}
                  </div>
                )}

                {/* Tool Calls - Compact display */}
                {message.toolCalls && message.toolCalls.length > 0 && (
                  <div className="ml-9 space-y-0.5">
                    {message.toolCalls.map((toolCall) => (
                      <ToolCallComponent
                        key={toolCall.id}
                        toolCall={toolCall}
                        onAccept={() => handleToolCallAccept(message.id, toolCall.id)}
                        onUndo={() => handleToolCallUndo(message.id, toolCall.id)}
                      />
                    ))}
                  </div>
                )}
              </div>
            )
          })}

          <div ref={messagesEndRef} />
        </div>
      </ScrollArea>

      <Separator className="bg-[#22272F]" />

      {/* Input Area - Minimal */}
      <div className="border-t border-[#22272F] bg-[#0F1115] p-3 space-y-2">
        {authRequired && (
          <div className="flex items-center justify-between gap-2 rounded-md border border-[#2C333D] bg-[#151A21] px-3 py-2">
            <div className="text-xs text-[#E7E9EC]">
              Authentication required. Open the portal token page and click <span className="font-semibold">“Send token to KiCad agent”</span>.
            </div>
            <div className="flex items-center gap-2 shrink-0">
              <Button
                size="sm"
                variant="outline"
                className="h-7 px-3 text-xs border-[#2C333D] bg-[#151A21] text-[#E7E9EC] hover:bg-[#1C222B]"
                onClick={() => {
                  const url = authRequired.tokenPageUrl || authRequired.loginUrl
                  if (!url) return
                  try {
                    window.open(url, "_blank", "noopener,noreferrer")
                  } catch {
                    // ignore
                  }
                }}
              >
                Login
              </Button>
            </div>
          </div>
        )}
        <div className="flex gap-2">
          <Input
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyPress={handleKeyPress}
            placeholder="Ask me anything..."
            disabled={false}
            className="flex-1 border-[#2C333D] bg-[#151A21] text-[#E7E9EC] placeholder:text-[#9AA3AD] focus:border-[#C7773A] focus:ring-[#C7773A]/40 h-9 text-[13px]"
          />
          {isLoading && (
            <Button
              onClick={stopStreaming}
              variant="outline"
              className="h-9 px-3 border-[#2C333D] bg-[#151A21] text-[#E7E9EC] hover:bg-[#1C222B]"
            >
              <X className="h-3.5 w-3.5 mr-1" />
              Stop
            </Button>
          )}
          <Button
            onClick={handleSend}
            disabled={!input.trim()}
            className="bg-[#C7773A] hover:bg-[#D08954] text-[#121417] border-0 h-9 px-3"
          >
            <Send className="h-3.5 w-3.5" />
          </Button>
        </div>
        {/* Model Selector and Accept/Undo All at Bottom */}
        <div className="flex items-center justify-between gap-2">
          <div className="flex items-center gap-2">
            <span className="text-xs text-[#9AA3AD]">Model:</span>
            <Select value={selectedModel} onValueChange={setSelectedModel}>
              <SelectTrigger className="h-7 w-[140px] border-[#2C333D] bg-[#151A21] text-xs text-[#E7E9EC]">
                <SelectValue />
              </SelectTrigger>
              <SelectContent className="bg-[#151A21] border-[#2C333D]">
                {MODELS.map((model) => (
                  <SelectItem
                    key={model.value}
                    value={model.value}
                    className="text-[#E7E9EC] focus:bg-[#C7773A] focus:text-[#121417] text-xs"
                  >
                    {model.label}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
          {(hasPendingToolCalls || hasAcceptedToolCalls) && (
            <div className="flex items-center gap-2">
              {hasPendingToolCalls && (
                <Button
                  size="sm"
                  variant="outline"
                  onClick={handleAcceptAll}
                  className="h-7 px-3 text-xs border-[#2C333D] bg-[#151A21] text-[#E7E9EC] hover:bg-[#C7773A] hover:text-[#121417] hover:border-[#C7773A]"
                >
                  <Check className="h-3 w-3 mr-1" />
                  Accept All
                </Button>
              )}
              {(hasPendingToolCalls || hasAcceptedToolCalls) && (
                <Button
                  size="sm"
                  variant="outline"
                  onClick={handleUndoAll}
                  className="h-7 px-3 text-xs border-[#2C333D] bg-[#151A21] text-[#E7E9EC] hover:bg-[#1C222B]"
                >
                  <X className="h-3 w-3 mr-1" />
                  Undo All
                </Button>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
