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

interface Message {
  id: string
  role: "user" | "assistant" | "thinking"
  content: string
  timestamp: Date
  toolCalls?: ToolCall[]
}

const MODELS = [
  { value: "google/gemma-3-27b-it:free", label: "Gemma 3 27B IT (Google)" },
]

const AGENT_API_URL = process.env.NEXT_PUBLIC_AGENT_API_URL || "http://127.0.0.1:5001/api/generate"

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
  const messagesEndRef = React.useRef<HTMLDivElement>(null)
  const queuedPromptRef = React.useRef<string | null>(null)
  const rpcWaitersRef = React.useRef(new Map<number, { resolve: (v: any) => void; reject: (e: any) => void }>())
  const rpcIdRef = React.useRef(0)

  const scrollToBottom = () => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" })
  }

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
      const resp = await sendRpcCommand("GET_SCHEMATIC_CONTEXT", { max_chars: 50000 })
      if (resp?.status === "OK" && typeof resp.data === "string" && resp.data.trim()) {
        return resp.data
      }
      return ""
    } catch (e) {
      return ""
    }
  }

  const handleToolCallAccept = (messageId: string, toolCallId: string) => {
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id === messageId) {
          const updatedToolCalls = msg.toolCalls?.map((tc) =>
            tc.id === toolCallId
              ? { ...tc, status: "accepted" as const }
              : tc
          )
          return { ...msg, toolCalls: updatedToolCalls }
        }
        return msg
      })
    )
  }

  const handleToolCallUndo = (messageId: string, toolCallId: string) => {
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id === messageId) {
          const updatedToolCalls = msg.toolCalls?.map((tc) =>
            tc.id === toolCallId
              ? { ...tc, status: "rejected" as const }
              : tc
          )
          return { ...msg, toolCalls: updatedToolCalls }
        }
        return msg
      })
    )
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
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id === latestMessageWithToolCalls.id) {
          const updatedToolCalls = msg.toolCalls?.map((tc) =>
            tc.status === "pending" ? { ...tc, status: "accepted" as const } : tc
          )
          return { ...msg, toolCalls: updatedToolCalls }
        }
        return msg
      })
    )
  }

  const handleUndoAll = () => {
    if (!latestMessageWithToolCalls) return
    setMessages((prev) =>
      prev.map((msg) => {
        if (msg.id === latestMessageWithToolCalls.id) {
          const updatedToolCalls = msg.toolCalls?.map((tc) =>
            tc.status === "accepted" || tc.status === "pending"
              ? { ...tc, status: "rejected" as const }
              : tc
          )
          return { ...msg, toolCalls: updatedToolCalls }
        }
        return msg
      })
    )
  }

  // Parse tool calls from response text
  const parseToolCalls = (text: string): ToolCall[] => {
    const toolCalls: ToolCall[] = []
    const lines = text.split('\n')
    
    for (const line of lines) {
      const trimmed = line.trim()
      if (trimmed.startsWith('TOOL ')) {
        const rest = trimmed.slice(5).trim()
        const spaceIdx = rest.indexOf(' ')
        if (spaceIdx === -1) continue
        
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
        } catch (e) {
          // Invalid JSON, skip this tool call
          console.warn('Failed to parse tool call JSON:', jsonStr)
        }
      }
    }
    
    return toolCalls
  }

  // Remove tool calls from text to get clean content
  const removeToolCalls = (text: string): string => {
    return text
      .split('\n')
      .filter(line => !line.trim().startsWith('TOOL '))
      .join('\n')
  }

  // Remove thinking tags (redacted_reasoning tags)
  const removeThinkingTags = (text: string): string => {
    return text
      .replace(/<think>/g, '')
      .replace(/<\/redacted_reasoning>/g, '')
      .trim()
  }

  const sendPrompt = async (promptText: string) => {
    const trimmedPrompt = promptText.trim()
    if (!trimmedPrompt) return

    const userMessage: Message = {
      id: Date.now().toString(),
      role: "user",
      content: trimmedPrompt,
      timestamp: new Date(),
    }

    setMessages((prev) => [...prev, userMessage])
    setIsLoading(true)

    try {
      // Include KiCad schematic context if available
      const schematicContext = await tryGetSchematicContext()
      const requestPrompt = schematicContext ? `${trimmedPrompt}\n\n${schematicContext}` : trimmedPrompt

      // Create assistant message that will be updated as we stream
      const assistantMessageId = `assistant-${Date.now()}`
      let accumulatedText = ""

      const assistantMessage: Message = {
        id: assistantMessageId,
        role: "assistant",
        content: "",
        timestamp: new Date(),
        toolCalls: [],
      }
      setMessages((prev) => [...prev, assistantMessage])

      const response = await fetch(AGENT_API_URL, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-Session-ID": sessionId,
        },
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

            if (responseText) {
              accumulatedText += responseText

              // Update message with accumulated text (but don't show tool calls yet)
              const cleanText = removeThinkingTags(accumulatedText)
              const contentWithoutTools = removeToolCalls(cleanText).trim()

              setMessages((prev) =>
                prev.map((msg) =>
                  msg.id === assistantMessageId
                    ? {
                        ...msg,
                        content: contentWithoutTools || "Processing...",
                      }
                    : msg
                )
              )
            }

            if (data.done) {
              const cleanText = removeThinkingTags(accumulatedText)
              const toolCalls = parseToolCalls(cleanText)

              if (toolCalls.length > 0) {
                console.info("Tool calls detected:", toolCalls)
                setMessages((prev) =>
                  prev.map((msg) =>
                    msg.id === assistantMessageId
                      ? {
                          ...msg,
                          toolCalls: toolCalls,
                        }
                      : msg
                  )
                )
              }
              break
            }
          } catch (e) {
            continue
          }
        }
      }
    } catch (error) {
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
      setIsLoading(false)

      // Auto-send queued prompt (if you typed ahead while streaming)
      const next = queuedPromptRef.current
      if (next && next.trim()) {
        queuedPromptRef.current = null
        setQueuedPrompt(null)
        // Fire-and-forget; avoid blocking UI
        setTimeout(() => void sendPrompt(next), 0)
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
        <div className="space-y-4 px-4 py-4">
          {messages.map((message) => {
            // Handle thinking messages
            if (message.role === "thinking") {
              return (
                <div key={message.id} className="flex gap-3 justify-start">
                  <Avatar className="h-6 w-6 shrink-0 border border-[#2C333D]">
                    <AvatarFallback className="bg-[#171B22] text-[#8B949E]">
                      <Bot className="h-3 w-3" />
                    </AvatarFallback>
                  </Avatar>
                  <div className="text-xs text-[#9AA3AD] font-mono py-1">
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
                    <p className="text-sm whitespace-pre-wrap leading-relaxed">
                      {message.content}
                    </p>
                  </div>
                  {message.role === "user" && (
                    <Avatar className="h-6 w-6 shrink-0 border border-[#2C333D]">
                      <AvatarFallback className="bg-[#171B22] text-[#E7E9EC]">
                        <User className="h-3 w-3" />
                      </AvatarFallback>
                    </Avatar>
                  )}
                </div>

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
        <div className="flex gap-2">
          <Input
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyPress={handleKeyPress}
            placeholder="Ask me anything..."
            disabled={false}
            className="flex-1 border-[#2C333D] bg-[#151A21] text-[#E7E9EC] placeholder:text-[#9AA3AD] focus:border-[#C7773A] focus:ring-[#C7773A]/40 h-9 text-sm"
          />
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
