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
  const [selectedModel, setSelectedModel] = React.useState("google/gemma-3-4b")
  const [sessionId] = React.useState(() => `session-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`)
  const messagesEndRef = React.useRef<HTMLDivElement>(null)

  const scrollToBottom = () => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" })
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

  const handleSend = async () => {
    if (!input.trim() || isLoading) return

    const userMessage: Message = {
      id: Date.now().toString(),
      role: "user",
      content: input.trim(),
      timestamp: new Date(),
    }

    setMessages((prev) => [...prev, userMessage])
    const prompt = input.trim()
    setInput("")
    setIsLoading(true)

    try {
      // Create assistant message that will be updated as we stream
      const assistantMessageId = `assistant-${Date.now()}`
      let accumulatedText = ""
      let thinkingOpen = false

      const assistantMessage: Message = {
        id: assistantMessageId,
        role: "assistant",
        content: "",
        timestamp: new Date(),
        toolCalls: [],
      }
      setMessages((prev) => [...prev, assistantMessage])

      // Call the agent API
      const response = await fetch(AGENT_API_URL, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-Session-ID": sessionId,
        },
        body: JSON.stringify({
          model: selectedModel,
          prompt: prompt,
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
        const lines = chunk.split('\n')

        for (const line of lines) {
          const trimmed = line.trim()
          if (!trimmed) continue

          try {
            const data = JSON.parse(trimmed)
            const responseText = data.response || ""

            if (responseText) {
              accumulatedText += responseText

              // Note: thinking tags are removed from display, but we track them for future use

              // Update message with accumulated text (but don't show tool calls yet)
              const cleanText = removeThinkingTags(accumulatedText)
              const contentWithoutTools = removeToolCalls(cleanText).trim()

              setMessages((prev) =>
                prev.map((msg) =>
                  msg.id === assistantMessageId
                    ? {
                        ...msg,
                        content: contentWithoutTools || "Processing...",
                        // Don't set toolCalls during streaming - only at the end
                      }
                    : msg
                )
              )
            }

            if (data.done) {
              // Parse and add tool calls only at the end
              const cleanText = removeThinkingTags(accumulatedText)
              const toolCalls = parseToolCalls(cleanText)
              
              if (toolCalls.length > 0) {
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
            // Skip invalid JSON lines
            continue
          }
        }
      }

      setIsLoading(false)
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
      setIsLoading(false)
    }
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
    <div className="flex h-screen flex-col bg-[#0A0A0A] text-[#E5E5E5]">
      {/* Header - Minimal */}
      <div className="border-b border-[#1A1A1A] bg-[#0A0A0A] px-4 py-2">
        <div className="flex items-center gap-2">
          <Avatar className="h-6 w-6 border border-[#2A2A2A]">
            <AvatarFallback className="bg-[#FF6B35] text-[#0A0A0A]">
              <Sparkles className="h-3 w-3" />
            </AvatarFallback>
          </Avatar>
          <h2 className="font-medium text-xs text-[#E5E5E5]">
            {isLoading ? "Thinking..." : "AI Assistant"}
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
                  <Avatar className="h-6 w-6 shrink-0 border border-[#2A2A2A]">
                    <AvatarFallback className="bg-[#2A2A2A] text-[#666666]">
                      <Bot className="h-3 w-3" />
                    </AvatarFallback>
                  </Avatar>
                  <div className="text-xs text-[#666666] font-mono py-1">
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
                    <Avatar className="h-6 w-6 shrink-0 border border-[#2A2A2A]">
                      <AvatarFallback className="bg-[#FF6B35] text-[#0A0A0A]">
                        <Bot className="h-3 w-3" />
                      </AvatarFallback>
                    </Avatar>
                  )}
                  <div
                    className={`max-w-[80%] rounded-lg px-3 py-2 ${
                      message.role === "user"
                        ? "bg-[#FF6B35] text-[#0A0A0A]"
                        : "bg-[#1A1A1A] border border-[#2A2A2A] text-[#E5E5E5]"
                    }`}
                  >
                    <p className="text-sm whitespace-pre-wrap leading-relaxed">
                      {message.content}
                    </p>
                  </div>
                  {message.role === "user" && (
                    <Avatar className="h-6 w-6 shrink-0 border border-[#2A2A2A]">
                      <AvatarFallback className="bg-[#2A2A2A] text-[#E5E5E5]">
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

      <Separator className="bg-[#1A1A1A]" />

      {/* Input Area - Minimal */}
      <div className="border-t border-[#1A1A1A] bg-[#0A0A0A] p-3 space-y-2">
        <div className="flex gap-2">
          <Input
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyPress={handleKeyPress}
            placeholder="Ask me anything..."
            disabled={isLoading}
            className="flex-1 border-[#2A2A2A] bg-[#1A1A1A] text-[#E5E5E5] placeholder:text-[#666666] focus:border-[#FF6B35] focus:ring-[#FF6B35] h-9 text-sm"
          />
          <Button
            onClick={handleSend}
            disabled={isLoading || !input.trim()}
            className="bg-[#FF6B35] hover:bg-[#FF7B45] text-[#0A0A0A] border-0 h-9 px-3"
          >
            <Send className="h-3.5 w-3.5" />
          </Button>
        </div>
        {/* Model Selector and Accept/Undo All at Bottom */}
        <div className="flex items-center justify-between gap-2">
          <div className="flex items-center gap-2">
            <span className="text-xs text-[#666666]">Model:</span>
            <Select value={selectedModel} onValueChange={setSelectedModel}>
              <SelectTrigger className="h-7 w-[140px] border-[#2A2A2A] bg-[#1A1A1A] text-xs text-[#E5E5E5]">
                <SelectValue />
              </SelectTrigger>
              <SelectContent className="bg-[#1A1A1A] border-[#2A2A2A]">
                {MODELS.map((model) => (
                  <SelectItem
                    key={model.value}
                    value={model.value}
                    className="text-[#E5E5E5] focus:bg-[#FF6B35] focus:text-[#0A0A0A] text-xs"
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
                  className="h-7 px-3 text-xs border-[#2A2A2A] bg-[#1A1A1A] text-[#E5E5E5] hover:bg-[#FF6B35] hover:text-[#0A0A0A] hover:border-[#FF6B35]"
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
                  className="h-7 px-3 text-xs border-[#2A2A2A] bg-[#1A1A1A] text-[#E5E5E5] hover:bg-[#2A2A2A]"
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
