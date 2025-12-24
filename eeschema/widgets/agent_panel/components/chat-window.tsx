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
  { value: "xiaomi/mimo-v2-flash", label: "MiMo-V2-Flash (Xiaomi)" },
  { value: "mistralai/devstral-2-2512", label: "Devstral 2 2512 (Mistral)" },
  { value: "kwaipilot/kat-coder-pro-v1", label: "KAT-Coder-Pro V1 (Kwaipilot)" },
  { value: "tng/deepseek-r1t2-chimera", label: "DeepSeek R1T2 Chimera (TNG)" },
  { value: "nvidia/nemotron-3-nano-30b-a3b", label: "Nemotron 3 Nano 30B (NVIDIA)" },
  { value: "nex-agi/deepseek-v3.1-nex-n1", label: "DeepSeek V3.1 Nex N1 (Nex AGI)" },
  { value: "tng/deepseek-r1t-chimera", label: "DeepSeek R1T Chimera (TNG)" },
  { value: "nvidia/nemotron-nano-12b-2-vl", label: "Nemotron Nano 12B 2 VL (NVIDIA)" },
  { value: "z-ai/glm-4.5-air", label: "GLM 4.5 Air (Z.AI)" },
  { value: "tng/r1t-chimera", label: "R1T Chimera (TNG)" },
  { value: "allenai/olmo-3-32b-think", label: "Olmo 3 32B Think (AllenAI)" },
  { value: "qwen/qwen2.5-vl-7b-instruct", label: "Qwen2.5-VL 7B Instruct (Qwen)" },
  { value: "meta-llama/llama-3.2-3b-instruct", label: "Llama 3.2 3B Instruct (Meta)" },
  { value: "qwen/qwen3-4b", label: "Qwen3 4B (Qwen)" },
  { value: "moonshotai/kimi-k2-0711", label: "Kimi K2 0711 (MoonshotAI)" },
  { value: "google/gemma-3-12b", label: "Gemma 3 12B (Google)" },
  { value: "google/gemma-3n-4b", label: "Gemma 3n 4B (Google)" },
  { value: "google/gemma-3n-2b", label: "Gemma 3n 2B (Google)" },
  { value: "google/gemma-3-4b", label: "Gemma 3 4B (Google)" },
]

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
  const [selectedModel, setSelectedModel] = React.useState("xiaomi/mimo-v2-flash")
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

  const handleSend = async () => {
    if (!input.trim() || isLoading) return

    const userMessage: Message = {
      id: Date.now().toString(),
      role: "user",
      content: input.trim(),
      timestamp: new Date(),
    }

    setMessages((prev) => [...prev, userMessage])
    setInput("")
    setIsLoading(true)

    // Add thinking message
    const thinkingMessage: Message = {
      id: `thinking-${Date.now()}`,
      role: "thinking",
      content: "Thinking...",
      timestamp: new Date(),
    }
    setMessages((prev) => [...prev, thinkingMessage])

    // Simulate AI response with tool calls (replace with actual API call)
    setTimeout(() => {
      // Remove thinking message
      setMessages((prev) => prev.filter((msg) => msg.id !== thinkingMessage.id))

      // Simulate tool calls for demonstration
      const mockToolCalls: ToolCall[] = [
        {
          id: `${Date.now()}-1`,
          name: "read_file",
          arguments: { path: "example.py", lines: "1-10" },
          status: "accepted",
          displayMode: "text", // Show as simple text
        },
        {
          id: `${Date.now()}-2`,
          name: "write_file",
          arguments: { path: "example.py", content: "# Updated content" },
          status: "pending",
          displayMode: "card", // Show as card (user can accept/undo)
        },
      ]

      const assistantMessage: Message = {
        id: (Date.now() + 1).toString(),
        role: "assistant",
        content: `I'll help you with: "${userMessage.content}". Here are the tool calls I'd like to make:`,
        timestamp: new Date(),
        toolCalls: mockToolCalls,
      }
      setMessages((prev) => [...prev, assistantMessage])
      setIsLoading(false)
    }, 1000)
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

                {/* Tool Calls */}
                {message.toolCalls && message.toolCalls.length > 0 && (
                  <div className="ml-9 space-y-1">
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
