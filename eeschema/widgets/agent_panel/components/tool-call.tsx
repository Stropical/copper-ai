"use client"

import * as React from "react"
import { Button } from "@/components/ui/button"
import { Check, X, Loader2 } from "lucide-react"
import { cn } from "@/lib/utils"

export interface ToolCall {
  id: string
  name: string
  arguments: string | Record<string, any>
  status: "pending" | "running" | "accepted" | "rejected"
  result?: string | Record<string, any>
  displayMode?: "card" | "text" // Whether to show as card or simple text
}

interface ToolCallProps {
  toolCall: ToolCall
  onAccept: (id: string) => void
  onUndo: (id: string) => void
}

export function ToolCallComponent({ toolCall, onAccept, onUndo }: ToolCallProps) {
  const isPending = toolCall.status === "pending"
  const isRunning = toolCall.status === "running"
  const isAccepted = toolCall.status === "accepted"
  const isRejected = toolCall.status === "rejected"

  // Extract a compact summary of arguments
  let argsSummary = ""
  if (typeof toolCall.arguments === "string") {
    argsSummary = toolCall.arguments.length > 50 
      ? toolCall.arguments.substring(0, 50) + "..." 
      : toolCall.arguments
  } else if (typeof toolCall.arguments === "object" && toolCall.arguments !== null) {
    // Create a compact summary
    const keys = Object.keys(toolCall.arguments)
    if (keys.length === 1) {
      argsSummary = `${keys[0]}: ${JSON.stringify(toolCall.arguments[keys[0]])}`
    } else {
      argsSummary = `${keys.length} args`
    }
    if (argsSummary.length > 50) {
      argsSummary = argsSummary.substring(0, 50) + "..."
    }
  }

  return (
    <div className={cn(
      "flex items-center gap-2 px-2 py-1 rounded border text-xs transition-all",
      "border-[#2C333D] bg-[#151A21] text-[#E7E9EC]",
      isAccepted && "border-[#C7773A]/50 bg-[#C7773A]/10",
      isRejected && "border-red-500/30 bg-red-500/5"
    )}>
      {/* Status indicator */}
      <div className="shrink-0">
        {isPending && <span className="text-[#9AA3AD]">○</span>}
        {isRunning && <Loader2 className="h-3 w-3 animate-spin text-[#C7773A]" />}
        {isAccepted && <Check className="h-3 w-3 text-[#C7773A]" />}
        {isRejected && <X className="h-3 w-3 text-red-500/70" />}
      </div>

      {/* Tool name and args */}
      <div className="flex-1 min-w-0">
        <span className="font-mono text-[#E7E9EC]">{toolCall.name}</span>
        {argsSummary && (
          <span className="text-[#9AA3AD] ml-2">{argsSummary}</span>
        )}
      </div>

      {/* Action buttons - only show for pending or accepted */}
      {(isPending || isAccepted) && (
        <div className="flex gap-1 shrink-0">
          {isPending && (
            <>
              <Button
                size="sm"
                variant="ghost"
                onClick={() => onAccept(toolCall.id)}
                className="h-5 w-5 p-0 hover:bg-[#C7773A]/20"
              >
                <Check className="h-3 w-3 text-[#C7773A]" />
              </Button>
              <Button
                size="sm"
                variant="ghost"
                onClick={() => onUndo(toolCall.id)}
                className="h-5 w-5 p-0 hover:bg-red-500/20"
              >
                <X className="h-3 w-3 text-red-500/70" />
              </Button>
            </>
          )}
          {isAccepted && (
            <Button
              size="sm"
              variant="ghost"
              onClick={() => onUndo(toolCall.id)}
              className="h-5 w-5 p-0 hover:bg-red-500/20"
            >
              <X className="h-3 w-3 text-red-500/70" />
            </Button>
          )}
        </div>
      )}
    </div>
  )
}

