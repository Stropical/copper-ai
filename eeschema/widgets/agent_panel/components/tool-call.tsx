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
  const isReadOnly = toolCall.name === "schematic.search_symbol" || toolCall.name === "schematic.get_datasheet"

  // Extract a compact summary of arguments
  let argsSummary = ""
  if (typeof toolCall.arguments === "string") {
    argsSummary = toolCall.arguments.length > 120
      ? toolCall.arguments.substring(0, 120) + "..."
      : toolCall.arguments
  } else if (typeof toolCall.arguments === "object" && toolCall.arguments !== null) {
    const args = toolCall.arguments as Record<string, any>

    // Special-case wiring to show something human readable
    if (args.from?.reference && args.from?.pin && args.to?.reference && args.to?.pin) {
      argsSummary = `${args.from.reference}:${args.from.pin} → ${args.to.reference}:${args.to.pin}`
    } else {
      // Fallback: compact JSON
      argsSummary = JSON.stringify(args)
    }

    if (argsSummary.length > 160) {
      argsSummary = argsSummary.substring(0, 160) + "..."
    }
  }

  return (
    <div className={cn(
      "flex flex-col gap-1 px-2 py-1 rounded border text-xs transition-all",
      "border-[#2C333D] bg-[#151A21] text-[#E7E9EC]",
      isAccepted && "border-[#C7773A]/50 bg-[#C7773A]/10",
      isRejected && "border-red-500/30 bg-red-500/5"
    )}>
      <div className="flex items-center gap-2">
        {/* Status indicator */}
        <div className="shrink-0">
          {isPending && <span className="text-[#9AA3AD]">○</span>}
          {isRunning && <Loader2 className="h-3 w-3 animate-spin text-[#C7773A]" />}
          {isAccepted && <Check className="h-3 w-3 text-[#C7773A]" />}
          {isRejected && <X className="h-3 w-3 text-red-500/70" />}
        </div>

        {/* Tool name and args */}
        <div className="flex-1 min-w-0">
          <div className="font-mono text-[#E7E9EC]">{toolCall.name}</div>
          {argsSummary && (
            <div className="text-[#9AA3AD] mt-1 text-[10px] leading-[1.5] whitespace-pre-wrap break-words">
              {argsSummary}
            </div>
          )}
        </div>

      {/* Action buttons - only show for pending or accepted (skip read-only tools) */}
      {!isReadOnly && (isPending || isAccepted) && (
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

      {/* Result (for query-style tools like get_datasheet) */}
      {toolCall.result && typeof toolCall.result === "string" && toolCall.result.trim() && (
        <div className="text-[#9AA3AD] font-mono text-[11px] whitespace-pre-wrap break-words">
          {toolCall.result}
        </div>
      )}
    </div>
  )
}

