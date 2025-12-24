"use client"

import * as React from "react"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { Badge } from "@/components/ui/badge"
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

// Simple text display for file reads and similar operations
function ToolCallText({ toolCall }: { toolCall: ToolCall }) {
  const argsDisplay = typeof toolCall.arguments === "string" 
    ? toolCall.arguments 
    : JSON.stringify(toolCall.arguments, null, 2)

  // Extract readable info from arguments
  let displayText = toolCall.name
  if (typeof toolCall.arguments === "object" && toolCall.arguments !== null) {
    if ("path" in toolCall.arguments) {
      displayText = `Read ${toolCall.arguments.path}`
    } else if ("command" in toolCall.arguments) {
      displayText = `Run: ${toolCall.arguments.command}`
    } else {
      displayText = `${toolCall.name}`
    }
  }

  const isRunning = toolCall.status === "running"
  const isAccepted = toolCall.status === "accepted"

  return (
    <div className="text-xs text-[#666666] font-mono py-0.5">
      {isRunning && <span className="text-[#FF6B35]">⏳ </span>}
      {isAccepted && <span className="text-[#FF6B35]/70">✓ </span>}
      {displayText}
    </div>
  )
}

export function ToolCallComponent({ toolCall, onAccept, onUndo }: ToolCallProps) {
  // Show as text for file reads and simple operations
  if (toolCall.displayMode === "text" || 
      (toolCall.name.includes("read") || toolCall.name.includes("file") || toolCall.name === "read_file")) {
    return <ToolCallText toolCall={toolCall} />
  }
  const argsDisplay = typeof toolCall.arguments === "string" 
    ? toolCall.arguments 
    : JSON.stringify(toolCall.arguments, null, 2)

  const resultDisplay = toolCall.result
    ? (typeof toolCall.result === "string" 
        ? toolCall.result 
        : JSON.stringify(toolCall.result, null, 2))
    : null

  const isPending = toolCall.status === "pending"
  const isRunning = toolCall.status === "running"
  const isAccepted = toolCall.status === "accepted"
  const isRejected = toolCall.status === "rejected"

  return (
    <Card className={cn(
      "w-full transition-all border-[#2A2A2A] bg-[#1A1A1A]",
      isAccepted && "border-[#FF6B35]/50 bg-[#FF6B35]/10",
      isRejected && "border-red-500/50 bg-red-500/10"
    )}>
      <CardHeader className="pb-3">
        <div className="flex items-start justify-between">
          <div className="space-y-1">
            <CardTitle className="text-sm font-medium flex items-center gap-2">
              <span className="font-mono text-xs">{toolCall.name}</span>
              {isRunning && (
                <Badge variant="secondary" className="text-xs">
                  <Loader2 className="h-3 w-3 mr-1 animate-spin" />
                  Running
                </Badge>
              )}
              {isAccepted && (
                <Badge variant="default" className="text-xs bg-[#FF6B35]/20 text-[#FF6B35] border-[#FF6B35]/50">
                  <Check className="h-3 w-3 mr-1" />
                  Accepted
                </Badge>
              )}
              {isRejected && (
                <Badge variant="destructive" className="text-xs">
                  <X className="h-3 w-3 mr-1" />
                  Rejected
                </Badge>
              )}
            </CardTitle>
            <CardDescription className="text-xs">
              Tool call
            </CardDescription>
          </div>
          {(isPending || isAccepted) && (
            <div className="flex gap-2">
              {isPending && (
                <>
                  <Button
                    size="sm"
                    variant="outline"
                    onClick={() => onAccept(toolCall.id)}
                    className="h-7 text-xs"
                  >
                    <Check className="h-3 w-3 mr-1" />
                    Accept
                  </Button>
                  <Button
                    size="sm"
                    variant="outline"
                    onClick={() => onUndo(toolCall.id)}
                    className="h-7 text-xs"
                  >
                    <X className="h-3 w-3 mr-1" />
                    Undo
                  </Button>
                </>
              )}
              {isAccepted && (
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => onUndo(toolCall.id)}
                  className="h-7 text-xs"
                >
                  <X className="h-3 w-3 mr-1" />
                  Undo
                </Button>
              )}
            </div>
          )}
        </div>
      </CardHeader>
      <CardContent className="space-y-3">
        <div>
          <div className="text-xs font-medium mb-1 text-muted-foreground">Arguments</div>
          <pre className="text-xs bg-muted/50 rounded p-2 overflow-x-auto font-mono">
            {argsDisplay}
          </pre>
        </div>
        {resultDisplay && (
          <div>
            <div className="text-xs font-medium mb-1 text-muted-foreground">Result</div>
            <pre className="text-xs bg-muted/50 rounded p-2 overflow-x-auto font-mono">
              {resultDisplay}
            </pre>
          </div>
        )}
      </CardContent>
    </Card>
  )
}

