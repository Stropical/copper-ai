"use client"

import * as React from "react"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Avatar, AvatarFallback } from "@/components/ui/avatar"
import { Separator } from "@/components/ui/separator"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select"
import { ToolCallComponent, ToolCall } from "@/components/tool-call"
import { Send, User, Bot, Sparkles, Check, X, Upload, CheckCircle2, XCircle } from "lucide-react"
import { Streamdown } from "streamdown"
import JSZip from "jszip"

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
  { value: "codex-5.1", label: "Codex 5.1" },
  { value: "google/gemma-3-27b-it:free", label: "Gemma 3 27B IT (Google)" },
  { value: "openai/gpt-oss-120b:free", label: "GPT-OSS 120B (OpenAI)" },
  { value: "qwen/qwen3-coder:free", label: "Qwen3 Coder (Qwen)"},
  { value: "meta-llama/llama-3.1-405b-instruct:free", label: "Llama 3.1 405B Instruct (Meta)"}
]

// Use agent_v3 by default, fallback to agent_v2
const AGENT_V3_URL = process.env.NEXT_PUBLIC_AGENT_V3_URL || "http://127.0.0.1:5002"
const AGENT_V2_URL = process.env.NEXT_PUBLIC_AGENT_API_URL || "http://127.0.0.1:5001"
const AGENT_API_URL = `${AGENT_V3_URL}/api/generate` // Use v3 by default
const AGENT_BASE_URL = AGENT_V2_URL.replace(/\/api\/generate\/?$/, "") // For v2 compatibility

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
  const [selectedModel, setSelectedModel] = React.useState("codex-5.1")
  const [sessionId] = React.useState(() => {
    // Try to get session ID from localStorage or generate new one
    if (typeof window === "undefined") {
      // SSR: return a temporary ID, will be replaced on client
      return `session-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`
    }
    const stored = localStorage.getItem("kicad_agent_session_id")
    if (stored) return stored
    const newId = `session-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`
    localStorage.setItem("kicad_agent_session_id", newId)
    return newId
  })
  const [authRequired, setAuthRequired] = React.useState<{
    loginUrl?: string
    tokenPageUrl?: string
  } | null>(null)
  const [authToken, setAuthToken] = React.useState<string | null>(null)
  const [debugLogs, setDebugLogs] = React.useState<Array<{ time: string; level: string; message: string }>>([])
  const [showDebugConsole, setShowDebugConsole] = React.useState(false)
  const [isUploading, setIsUploading] = React.useState(false)
  const [pendingChanges, setPendingChanges] = React.useState<{ added: number; removed: number } | null>(null)
  const fileInputRef = React.useRef<HTMLInputElement>(null)
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

  // Debug logging helper (directly adds to logs without going through console to avoid recursion)
  const addDebugLog = React.useCallback((level: string, message: string) => {
    const time = new Date().toLocaleTimeString()
    setDebugLogs((prev) => [...prev.slice(-49), { time, level, message }]) // Keep last 50 logs
    // Don't call console methods here to avoid recursion
  }, [])

  // Intercept console methods (with recursion guard)
  React.useEffect(() => {
    let isIntercepting = false
    const originalLog = console.log
    const originalError = console.error
    const originalWarn = console.warn

    console.log = (...args: any[]) => {
      originalLog(...args)
      if (!isIntercepting) {
        isIntercepting = true
        try {
          const message = args.map(a => {
            if (typeof a === "object") {
              try {
                return JSON.stringify(a)
              } catch {
                return String(a)
              }
            }
            return String(a)
          }).join(" ")
          const time = new Date().toLocaleTimeString()
          setDebugLogs((prev) => [...prev.slice(-49), { time, level: "log", message }])
        } finally {
          isIntercepting = false
        }
      }
    }

    console.error = (...args: any[]) => {
      originalError(...args)
      if (!isIntercepting) {
        isIntercepting = true
        try {
          const message = args.map(a => {
            if (typeof a === "object") {
              try {
                return JSON.stringify(a)
              } catch {
                return String(a)
              }
            }
            return String(a)
          }).join(" ")
          const time = new Date().toLocaleTimeString()
          setDebugLogs((prev) => [...prev.slice(-49), { time, level: "error", message }])
        } finally {
          isIntercepting = false
        }
      }
    }

    console.warn = (...args: any[]) => {
      originalWarn(...args)
      if (!isIntercepting) {
        isIntercepting = true
        try {
          const message = args.map(a => {
            if (typeof a === "object") {
              try {
                return JSON.stringify(a)
              } catch {
                return String(a)
              }
            }
            return String(a)
          }).join(" ")
          const time = new Date().toLocaleTimeString()
          setDebugLogs((prev) => [...prev.slice(-49), { time, level: "warn", message }])
        } finally {
          isIntercepting = false
        }
      }
    }

    return () => {
      console.log = originalLog
      console.error = originalError
      console.warn = originalWarn
    }
  }, [])

  // Validate auth token on mount
  React.useEffect(() => {
    const validateAuth = async () => {
      if (typeof window === "undefined") return
      const storedToken = localStorage.getItem("copper_auth_token")
      if (!storedToken) {
        addDebugLog("warn", "No auth token found")
        setAuthRequired({
          tokenPageUrl: "https://portal.copper.ai/token",
        })
        return
      }

      setAuthToken(storedToken)
      addDebugLog("log", "Auth token found, validating...")

      // Try to validate token by making a lightweight test request
      // Use a timeout to avoid blocking the UI
      try {
        const controller = new AbortController()
        const timeoutId = setTimeout(() => controller.abort(), 3000) // 3 second timeout

        const testResp = await fetch(`${AGENT_BASE_URL}/api/generate`, {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "Authorization": `Bearer ${storedToken}`,
          },
          body: JSON.stringify({
            model: "google/gemma-3-27b-it:free",
            prompt: "test",
            stream: false,
          }),
          signal: controller.signal,
        })

        clearTimeout(timeoutId)

        if (testResp.status === 401) {
          addDebugLog("error", "Auth token is invalid or expired")
          setAuthRequired({
            tokenPageUrl: "https://portal.copper.ai/token",
          })
          if (typeof window !== "undefined") {
            localStorage.removeItem("copper_auth_token")
          }
          setAuthToken(null)
        } else {
          addDebugLog("log", "Auth token validated successfully")
          setAuthRequired(null)
        }
      } catch (e: any) {
        // If server is not available or request timed out, check if it's an auth error
        if (e.name === "AbortError") {
          addDebugLog("warn", "Auth validation timed out - assuming token is valid")
        } else {
          addDebugLog("warn", `Could not validate token (server may be offline): ${e.message || e}`)
        }
        // Don't block UI - assume token is OK if we can't validate
        // User will get auth error on first real request if token is invalid
        setAuthRequired(null)
      }
    }

    validateAuth()
  }, [addDebugLog])

  // Listen for auth token from portal (via postMessage)
  React.useEffect(() => {
    const handleMessage = async (event: MessageEvent) => {
      try {
        const data = typeof event.data === "string" ? JSON.parse(event.data) : event.data
        if (data && typeof data === "object" && data.type === "copper_auth_token" && data.token) {
          console.log("[agent_panel] Received auth token from portal")
          
          // Store in state and localStorage
          setAuthToken(data.token)
          if (typeof window !== "undefined") {
            localStorage.setItem("copper_auth_token", data.token)
          }
          setAuthRequired(null)
          
          // Also send to agent server for compatibility
          try {
            await fetch(`${AGENT_BASE_URL}/api/auth/set-token`, {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                token: data.token,
                access_token: data.token,
                refresh_token: data.refresh_token,
              }),
            })
          } catch (e) {
            console.warn("[agent_panel] Failed to send token to server:", e)
            // Non-critical - token is stored locally and will be sent per-request
          }
          
          // Show success message
          setMessages((prev) => [
            ...prev,
            {
              id: `auth-success-${Date.now()}`,
              role: "assistant",
              content: "✅ Authentication successful! You can now continue using the agent.",
              timestamp: new Date(),
            },
          ])
        }
      } catch (e) {
        // Ignore parse errors
      }
    }

    window.addEventListener("message", handleMessage)
    return () => window.removeEventListener("message", handleMessage)
  }, [])

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

        // Handle pending changes notification (not an RPC response)
        if (payload.type === "PENDING_CHANGES_NOTIFICATION") {
          setPendingChanges({
            added: payload.added || 0,
            removed: payload.removed || 0,
          })
          return
        }

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

  const sendRpcCommand = React.useCallback(async (command: string, parameters?: Record<string, any>) => {
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

      // Increased timeout to 10 seconds for project path retrieval
      const timeout = command === "GET_PROJECT_PATH" ? 10000 : 5000
      window.setTimeout(() => {
        if (rpcWaitersRef.current.has(messageId)) {
          rpcWaitersRef.current.delete(messageId)
          reject(new Error(`KiCad RPC timeout after ${timeout}ms`))
        }
      }, timeout)
    })
  }, [postToKiCad])

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

  const tryGetProjectPath = async (): Promise<string | null> => {
    try {
      addDebugLog("log", "Attempting to get project path from KiCad...")
      
      // Check if RPC bridge is available by testing postToKiCad
      const testPayload = JSON.stringify({ command: "PING", message_id: 0, parameters: {} })
      const bridgeAvailable = postToKiCad(testPayload)
      
      if (!bridgeAvailable) {
        addDebugLog("warn", "KiCad RPC bridge not available - window.webkit, window.chrome.webview, or window.external not found")
        return null
      }

      addDebugLog("log", "KiCad RPC bridge detected, sending GET_PROJECT_PATH command...")
      
      const resp = await sendRpcCommand("GET_PROJECT_PATH", {})
      
      addDebugLog("log", `GET_PROJECT_PATH response: ${JSON.stringify(resp)}`)
      
      if (resp?.status === "OK" && typeof resp.data === "string" && resp.data.trim()) {
        const path = resp.data.trim()
        addDebugLog("log", `✓ Project path retrieved: ${path}`)
        return path
      }
      
      if (resp?.status === "ERROR") {
        addDebugLog("warn", `GET_PROJECT_PATH error: ${resp.error_message || "Unknown error"}`)
        if (resp.error_message?.includes("No project")) {
          addDebugLog("warn", "No project is currently open in KiCad")
        }
      } else if (resp?.status) {
        addDebugLog("warn", `GET_PROJECT_PATH returned status: ${resp.status}, data: ${JSON.stringify(resp.data)}`)
      } else {
        addDebugLog("warn", "GET_PROJECT_PATH returned no response - command may have timed out")
      }
      
      return null
    } catch (e: any) {
      addDebugLog("error", `Failed to get project path: ${e.message || e}`)
      if (e.message?.includes("timeout")) {
        addDebugLog("warn", "RPC command timed out - KiCad may be busy or not responding")
      }
      return null
    }
  }

  const handleUploadSchematic = React.useCallback(async (file: File) => {
    setIsUploading(true)
    addDebugLog("log", `Uploading schematic file: ${file.name}`)
    
    try {
      // Read the file as ArrayBuffer and convert to base64
      const reader = new FileReader()
      reader.onload = async (e) => {
        try {
          const arrayBuffer = e.target?.result as ArrayBuffer
          const bytes = new Uint8Array(arrayBuffer)
          const binary = String.fromCharCode(...bytes)
          const base64 = btoa(binary)
          
          addDebugLog("log", `File read successfully, size: ${file.size} bytes`)
          
          // Try to call REPLACE_SCHEMATIC_FROM_DATA RPC command
          // This will need to be implemented in KiCad to accept base64 file content
          const resp = await sendRpcCommand("REPLACE_SCHEMATIC_FROM_DATA", {
            filename: file.name,
            file_content: base64,
            skip_undo: false,
            skip_set_dirty: false,
          })
          
          if (resp?.status === "OK") {
            addDebugLog("log", `✓ Schematic file uploaded successfully: ${file.name}`)
            setMessages((prev) => [
              ...prev,
              {
                id: `upload-${Date.now()}`,
                role: "user",
                content: `Uploaded schematic file: ${file.name}`,
                timestamp: new Date(),
              },
              {
                id: `upload-response-${Date.now()}`,
                role: "assistant",
                content: `Successfully loaded schematic file "${file.name}". The schematic has been replaced in KiCad.`,
                timestamp: new Date(),
              },
            ])
          } else {
            const errorMsg = resp?.error_message || "Unknown error"
            addDebugLog("error", `Failed to upload schematic: ${errorMsg}`)
            setMessages((prev) => [
              ...prev,
              {
                id: `upload-${Date.now()}`,
                role: "user",
                content: `Tried to upload schematic file: ${file.name}`,
                timestamp: new Date(),
              },
              {
                id: `upload-response-${Date.now()}`,
                role: "assistant",
                content: `Failed to upload schematic file: ${errorMsg}. Please try using KiCad's File > Open menu instead.`,
                timestamp: new Date(),
              },
            ])
          }
        } catch (err: any) {
          addDebugLog("error", `Failed to process file: ${err.message}`)
          setMessages((prev) => [
            ...prev,
            {
              id: `upload-error-${Date.now()}`,
              role: "assistant",
              content: `Error uploading file: ${err.message}. Please try using KiCad's File > Open menu instead.`,
              timestamp: new Date(),
            },
          ])
        } finally {
          setIsUploading(false)
        }
      }
      reader.onerror = () => {
        addDebugLog("error", "Failed to read file")
        setIsUploading(false)
      }
      reader.readAsArrayBuffer(file)
    } catch (err: any) {
      addDebugLog("error", `Failed to read file: ${err.message}`)
      setIsUploading(false)
    }
  }, [addDebugLog, setMessages, sendRpcCommand])

  const handleFileSelect = React.useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    if (file) {
      handleUploadSchematic(file)
    }
    // Reset the input so the same file can be selected again
    if (fileInputRef.current) {
      fileInputRef.current.value = ""
    }
  }, [handleUploadSchematic])

  // Extract user ID from JWT token or generate one
  const getUserId = (): string => {
    // Try to get from localStorage first
    let storedUserId = localStorage.getItem("copper_user_id")
    if (storedUserId) {
      return storedUserId
    }

    // Try to extract from JWT token (check both state and localStorage)
    const token = authToken || localStorage.getItem("copper_auth_token")
    if (token) {
      try {
        const parts = token.split(".")
        if (parts.length === 3) {
          const payload = JSON.parse(atob(parts[1]))
          if (payload.sub || payload.user_id || payload.id) {
            const userId = payload.sub || payload.user_id || payload.id
            localStorage.setItem("copper_user_id", userId)
            return userId
          }
        }
      } catch (e) {
        console.warn("[agent_panel] Failed to decode JWT:", e)
      }
    }

    // Generate a new user ID
    const newUserId = `user-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`
    localStorage.setItem("copper_user_id", newUserId)
    return newUserId
  }

  // Zip project and upload to agent_v3 server
  const zipAndUploadProject = React.useCallback(async (): Promise<void> => {
    try {
      addDebugLog("log", "Starting project zip and upload...")
      
      // Retry getting project path a few times with delays
      let projectPath: string | null = null
      for (let attempt = 1; attempt <= 3; attempt++) {
        addDebugLog("log", `Attempt ${attempt}/3 to get project path...`)
        projectPath = await tryGetProjectPath()
        if (projectPath) {
          break
        }
        if (attempt < 3) {
          addDebugLog("log", `Waiting 1 second before retry...`)
          await new Promise(resolve => setTimeout(resolve, 1000))
        }
      }

      if (!projectPath) {
        addDebugLog("warn", "No project path available after 3 attempts - project may not be open in KiCad")
        addDebugLog("warn", "Upload skipped. Make sure a KiCad project is open.")
        return
      }

      addDebugLog("log", `Project path: ${projectPath}`)

      // Try to get zip from KiCad via RPC first
      let zipBlob: Blob | null = null
      try {
        const zipResp = await sendRpcCommand("ZIP_PROJECT", { project_path: projectPath })
        if (zipResp?.status === "OK" && zipResp.data) {
          // If KiCad returns base64 encoded zip
          if (typeof zipResp.data === "string") {
            const binaryString = atob(zipResp.data)
            const bytes = new Uint8Array(binaryString.length)
            for (let i = 0; i < binaryString.length; i++) {
              bytes[i] = binaryString.charCodeAt(i)
            }
            zipBlob = new Blob([bytes], { type: "application/zip" })
            addDebugLog("log", `Got zip from KiCad RPC (${zipBlob.size} bytes)`)
          }
        }
      } catch (e) {
        addDebugLog("log", `ZIP_PROJECT RPC not available, will zip manually: ${e}`)
      }

      // If RPC didn't work, try to get project files and zip manually
      if (!zipBlob) {
        try {
          const filesResp = await sendRpcCommand("GET_PROJECT_FILES", { project_path: projectPath })
          if (filesResp?.status === "OK" && Array.isArray(filesResp.data)) {
            const zip = new JSZip()
            const files = filesResp.data as Array<{ path: string; content: string }>
            
            for (const file of files) {
              zip.file(file.path, file.content)
            }
            
            const generatedBlob = await zip.generateAsync({ type: "blob" })
            zipBlob = generatedBlob
            addDebugLog("log", `Created zip manually from project files (${generatedBlob.size} bytes)`)
          }
        } catch (e) {
          addDebugLog("warn", `Failed to get project files: ${e}`)
        }
      }

      // If still no zip, try to get a list of files and request them individually
      if (!zipBlob) {
        try {
          const listResp = await sendRpcCommand("LIST_PROJECT_FILES", { project_path: projectPath })
          if (listResp?.status === "OK" && Array.isArray(listResp.data)) {
            const zip = new JSZip()
            const filePaths = listResp.data as string[]
            
            // Request each file
            for (const filePath of filePaths) {
              try {
                const fileResp = await sendRpcCommand("GET_FILE", { file_path: filePath })
                if (fileResp?.status === "OK" && fileResp.data) {
                  const content = typeof fileResp.data === "string" ? fileResp.data : JSON.stringify(fileResp.data)
                  // Use relative path from project directory
                  const relativePath = filePath.startsWith(projectPath) 
                    ? filePath.substring(projectPath.length + 1)
                    : filePath
                  zip.file(relativePath, content)
                }
              } catch (e) {
                addDebugLog("warn", `Failed to get file ${filePath}: ${e}`)
              }
            }
            
            const generatedBlob = await zip.generateAsync({ type: "blob" })
            zipBlob = generatedBlob
            addDebugLog("log", `Created zip from individual file requests (${generatedBlob.size} bytes)`)
          }
        } catch (e) {
          addDebugLog("warn", `Failed to list/get project files: ${e}`)
          return
        }
      }

      if (!zipBlob) {
        addDebugLog("error", "Could not create project zip - all methods failed")
        return
      }

      // Upload to agent_v3 server
      const userId = getUserId()
      addDebugLog("log", `Uploading to ${AGENT_V3_URL}/api/upload-project (user: ${userId}, session: ${sessionId})`)
      
      const formData = new FormData()
      formData.append("project", zipBlob, "project.zip")
      if (projectPath) {
        formData.append("projectPath", projectPath)
      }

      try {
        const uploadResp = await fetch(`${AGENT_V3_URL}/api/upload-project`, {
          method: "POST",
          headers: {
            "X-User-ID": userId,
            "X-Session-ID": sessionId,
          },
          body: formData,
        })

        if (!uploadResp.ok) {
          const errorText = await uploadResp.text()
          throw new Error(`Upload failed: ${uploadResp.status} ${errorText}`)
        }

        const result = await uploadResp.json()
        addDebugLog("log", `Project uploaded successfully: ${JSON.stringify(result)}`)
      } catch (uploadError: any) {
        addDebugLog("error", `Upload failed: ${uploadError.message || uploadError}`)
        // Check if it's a network error
        if (uploadError.message?.includes("fetch") || uploadError.message?.includes("Failed to fetch")) {
          addDebugLog("error", `Cannot reach agent_v3 server at ${AGENT_V3_URL}. Is it running?`)
        }
        throw uploadError
      }
    } catch (e: any) {
      addDebugLog("error", `Failed to zip and upload project: ${e.message || e}`)
      // Don't show error to user, just log it
    }
  }, [sendRpcCommand, addDebugLog, sessionId, getUserId])

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
      let respData = resp.data
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

      // Check if the tool result contains a schematic file path and automatically replace the schematic
      if (ok && typeof respData === "string" && respData.trim()) {
        // Look for schematic file paths in the response (common patterns)
        const schematicFilePatterns = [
          /(?:file[_\s]*path|saved[_\s]*to|output[_\s]*file)[:\s]+([^\s]+\.kicad_sch)/i,
          /([^\s]+\.kicad_sch)/i, // Any .kicad_sch file path
        ]

        for (const pattern of schematicFilePatterns) {
          const match = respData.match(pattern)
          if (match && match[1]) {
            const filePath = match[1].trim()
            
            // Check if it's an absolute path or relative to project
            let fullPath = filePath
            if (!filePath.startsWith("/") && !filePath.match(/^[A-Za-z]:/)) {
              // Relative path - try to get project path and make it absolute
              try {
                const projectPathResp = await sendRpcCommand("GET_PROJECT_PATH", {})
                if (projectPathResp?.status === "OK" && projectPathResp.data) {
                  const projectPath = projectPathResp.data as string
                  // Simple path joining (assuming Unix-style paths)
                  fullPath = projectPath.endsWith("/") 
                    ? `${projectPath}${filePath}` 
                    : `${projectPath}/${filePath}`
                }
              } catch (e) {
                console.warn("Failed to get project path for relative schematic file:", e)
              }
            }

            // Try to replace the schematic
            try {
              addDebugLog("log", `Detected schematic file in tool result: ${fullPath}, attempting to replace...`)
              const replaceResp = await sendRpcCommand("REPLACE_SCHEMATIC", {
                file_path: fullPath,
                skip_undo: false, // Keep undo history
                skip_set_dirty: false, // Mark as modified
              })

              if (replaceResp?.status === "OK") {
                addDebugLog("log", `Successfully replaced schematic from: ${fullPath}`)
                // Update the tool result to indicate the schematic was replaced
                respData = `${respData}\n\n✅ Schematic replaced in RAM from: ${fullPath}`
              } else {
                addDebugLog("warn", `Failed to replace schematic: ${replaceResp?.error_message || "Unknown error"}`)
              }
            } catch (e: any) {
              addDebugLog("error", `Error replacing schematic: ${e?.message || "Unknown error"}`)
            }
            break // Only process the first match
          }
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
      thinkingText: thinking, // Don't trim to preserve whitespace
      thinkingOpen: open,
    }
  }

  // Poll for pending tool calls from Codex agent (via MCP server)
  // This allows Codex (running on server) to call KiCad tools via agent_panel
  React.useEffect(() => {
    if (!sessionId) return

    const pollInterval = setInterval(async () => {
      try {
        const resp = await fetch(`${AGENT_V3_URL}/api/kicad/pending-tools?sessionId=${sessionId}`)
        if (!resp.ok) return

        const data = await resp.json()
        if (!data.success || !data.pending || data.pending.length === 0) return

        // Process each pending tool call
        for (const call of data.pending) {
          try {
            addDebugLog("log", `Executing tool call from Codex: ${call.tool_name}`)

            // Execute tool call via KiCad RPC
            const result = await sendRpcCommand("RUN_TOOL", {
              tool_name: call.tool_name,
              payload: call.payload,
            })

            // Send result back to agent_v3 server
            await fetch(`${AGENT_V3_URL}/api/kicad/tool-result`, {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                callId: call.id,
                result: result.status === "OK" ? result.data : null,
                error: result.status === "ERROR" ? result.error_message : null,
              }),
            })

            addDebugLog("log", `Tool call ${call.tool_name} completed`)
          } catch (e: any) {
            addDebugLog("error", `Error executing tool call ${call.tool_name}: ${e?.message || e}`)
            
            // Send error back
            try {
              await fetch(`${AGENT_V3_URL}/api/kicad/tool-result`, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  callId: call.id,
                  result: null,
                  error: e?.message || "Unknown error",
                }),
              })
            } catch (sendError) {
              // Ignore send errors
            }
          }
        }
      } catch (e) {
        // Ignore polling errors (server might be down, etc.)
      }
    }, 1000) // Poll every second

    return () => clearInterval(pollInterval)
  }, [sessionId, sendRpcCommand, addDebugLog])

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

      // Get project path for the agent
      const projectPath = await tryGetProjectPath()

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

      const headers: Record<string, string> = {
        "Content-Type": "application/json",
        "X-Session-ID": sessionId,
      }

      // Add Authorization header if we have a token
      if (authToken) {
        headers["Authorization"] = `Bearer ${authToken}`
      }

      // Use agent_v3 format (sessionId in body, no stream/project_path needed)
      const response = await fetch(AGENT_API_URL, {
        method: "POST",
        headers,
        signal: abortController.signal,
        body: JSON.stringify({
          model: selectedModel,
          prompt: requestPrompt,
          sessionId: sessionId, // agent_v3 uses sessionId (not session_id)
        }),
      })

      // Handle authentication errors before streaming
      if (response.status === 401) {
        try {
          const errorData = await response.json()
          if (errorData.auth_required) {
            setAuthRequired({
              loginUrl: errorData.login_url,
              tokenPageUrl: errorData.token_page_url || "https://portal.copper.ai/token",
            })
            setMessages((prev) => [
              ...prev,
              {
                id: `error-${Date.now()}`,
                role: "assistant",
                content: "❌ Authentication required. Please log in to continue.",
                timestamp: new Date(),
              },
            ])
            return
          }
        } catch {
          // If parsing fails, show generic auth error
          setAuthRequired({
            tokenPageUrl: "https://portal.copper.ai/token",
          })
          setMessages((prev) => [
            ...prev,
            {
              id: `error-${Date.now()}`,
              role: "assistant",
              content: "❌ Authentication required. Please log in to continue.",
              timestamp: new Date(),
            },
          ])
          return
        }
      }

      if (!response.ok) {
        let errorMessage = `API error: ${response.status} ${response.statusText}`;
        let requiresReupload = false;
        try {
          const errorData = await response.json();
          if (errorData.error || errorData.message) {
            errorMessage = errorData.error || errorData.message;
          }
          if (errorData.requiresReupload) {
            requiresReupload = true;
          }
          addDebugLog("error", `API returned ${response.status}: ${JSON.stringify(errorData)}`);
        } catch (e) {
          const errorText = await response.text().catch(() => "");
          if (errorText) {
            errorMessage += ` - ${errorText}`;
            addDebugLog("error", `API returned ${response.status}: ${errorText}`);
          }
        }
        
        // If we need to re-upload, try to do it automatically
        if (requiresReupload) {
          addDebugLog("warn", "Project directory not found, attempting to re-upload project...");
          try {
            await zipAndUploadProject();
            addDebugLog("log", "✅ Project re-uploaded successfully, retrying request...");
            // Retry the request after a short delay
            const retryPrompt = promptText;
            const retrySilent = silent;
            setTimeout(() => {
              void sendPrompt(retryPrompt, retrySilent);
            }, 500);
            return;
          } catch (uploadError: any) {
            addDebugLog("error", `Failed to re-upload project: ${uploadError.message || uploadError}`);
            setMessages((prev) => [
              ...prev,
              {
                id: `error-${Date.now()}`,
                role: "assistant",
                content: `❌ Error: Project directory not found and automatic re-upload failed. Please try again.`,
                timestamp: new Date(),
              },
            ]);
            return;
          }
        }
        
        throw new Error(errorMessage);
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

            // Check if server sent a schematic file path to apply
            if (data.schematic_file && typeof data.schematic_file === "string") {
              const schematicPath = data.schematic_file.trim()
              addDebugLog("log", `Server sent schematic file path: ${schematicPath}`)
              
              // Apply the schematic (will highlight changes and show buttons)
              setTimeout(async () => {
                try {
                  const replaceResp = await sendRpcCommand("REPLACE_SCHEMATIC", {
                    file_path: schematicPath,
                    skip_undo: false,
                    skip_set_dirty: false,
                  })
                  
                  if (replaceResp?.status === "OK") {
                    addDebugLog("log", `✅ Successfully applied schematic from agent: ${schematicPath}`)
                    
                    // Get pending changes count to show buttons
                    try {
                      const changesResp = await sendRpcCommand("GET_PENDING_CHANGES", {})
                      if (changesResp?.status === "OK" && changesResp.data) {
                        setPendingChanges(changesResp.data as { added: number; removed: number })
                        addDebugLog("log", `Pending changes: ${changesResp.data.added} added, ${changesResp.data.removed} removed`)
                      }
                    } catch (e) {
                      // Ignore - changes might not be tracked yet
                    }
                    
                    setMessages((prev) =>
                      prev.map((msg) =>
                        msg.id === assistantMessageId
                          ? {
                              ...msg,
                              content: `${msg.content}\n\n✅ Schematic changes applied. Review and click Apply or Discard.`,
                            }
                          : msg
                      )
                    )
                  } else {
                    const errorMsg = replaceResp?.error_message || "Unknown error"
                    addDebugLog("warn", `Failed to apply schematic: ${errorMsg}`)
                    setMessages((prev) =>
                      prev.map((msg) =>
                        msg.id === assistantMessageId
                          ? {
                              ...msg,
                              content: `${msg.content}\n\n❌ Failed to apply schematic:\n${errorMsg}`,
                            }
                          : msg
                      )
                    )
                  }
                } catch (e: any) {
                  const errorMsg = e?.message || "Unknown error"
                  addDebugLog("error", `Error applying schematic: ${errorMsg}`)
                  setMessages((prev) =>
                    prev.map((msg) =>
                      msg.id === assistantMessageId
                        ? {
                            ...msg,
                            content: `${msg.content}\n\n❌ Error applying schematic: ${errorMsg}`,
                          }
                        : msg
                    )
                  )
                }
              }, 100)
            }

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
                // Add line breaks between thinking statements for better readability
                // Break on camelCase transitions and sentence endings
                const formattedThinking = thinkingText
                  .replace(/([a-z])([A-Z])/g, '$1\n$2') // Break camelCase: "ReadingFile" -> "Reading\nFile"
                  .replace(/([.!?])\s*([A-Z])/g, '$1\n\n$2') // Break on sentence endings
                  .split('\n')
                  .map(line => line.trim())
                  .filter(line => line.length > 0)
                  .join('\n')
                
                const displayThinking = thinkingOpen
                  ? `Thinking…\n\n${formattedThinking}`
                  : formattedThinking

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
                        content: thinkingText || "", // Preserve whitespace
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
      
      // Check if error response indicates we need to re-upload the project
      let requiresReupload = false
      if (error instanceof Error && 'response' in error) {
        try {
          const errorResponse = (error as any).response
          if (errorResponse && typeof errorResponse.json === 'function') {
            const errorData = await errorResponse.json()
            if (errorData.requiresReupload) {
              requiresReupload = true
            }
          }
        } catch (e) {
          // Ignore JSON parse errors
        }
      }
      
      // If we need to re-upload, try to do it automatically
      if (requiresReupload) {
        addDebugLog("warn", "Project directory not found, attempting to re-upload project...")
        try {
          await zipAndUploadProject()
          addDebugLog("log", "✅ Project re-uploaded successfully, retrying request...")
          // Retry the request after a short delay
          const retryPrompt = promptText;
          const retrySilent = silent;
          setTimeout(() => {
            void sendPrompt(retryPrompt, retrySilent);
          }, 500);
          return
        } catch (uploadError: any) {
          addDebugLog("error", `Failed to re-upload project: ${uploadError.message || uploadError}`)
          setMessages((prev) => [
            ...prev,
            {
              id: `error-${Date.now()}`,
              role: "assistant",
              content: `❌ Error: Project directory not found and automatic re-upload failed. Please try again.`,
              timestamp: new Date(),
            },
          ])
          return
        }
      }
      
      const errorMessage = error instanceof Error ? error.message : String(error)
      const errorDetails = error instanceof Error && error.stack ? error.stack : ""
      console.error("Error calling agent API:", error)
      addDebugLog("error", `Agent API error: ${errorMessage}${errorDetails ? `\n${errorDetails}` : ""}`)
      setMessages((prev) => [
        ...prev,
        {
          id: `error-${Date.now()}`,
          role: "assistant",
          content: `❌ Error: ${errorMessage}. Check the debug console for details.`,
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
                    className={`min-w-0 max-w-[80%] text-[11px] leading-[1.6] text-[#7C8590] font-mono py-2 px-2 whitespace-pre-wrap break-words ${
                      isLoading ? "animate-pulse" : ""
                    }`}
                  >
                    {message.content.split(/\n+/).map((line, idx, arr) => (
                      <React.Fragment key={idx}>
                        {line}
                        {idx < arr.length - 1 && <br />}
                      </React.Fragment>
                    ))}
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
                  <div className="ml-9 space-y-2">
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
          <input
            ref={fileInputRef}
            type="file"
            accept=".kicad_sch,.sch"
            onChange={handleFileSelect}
            className="hidden"
            id="schematic-file-input"
          />
          <Button
            onClick={() => fileInputRef.current?.click()}
            disabled={isUploading}
            variant="outline"
            className="h-9 px-3 border-[#2C333D] bg-[#151A21] text-[#E7E9EC] hover:bg-[#1C222B]"
            title="Upload schematic file"
          >
            <Upload className={`h-3.5 w-3.5 ${isUploading ? "animate-spin" : ""}`} />
          </Button>
          {pendingChanges && (
            <>
              <Button
                onClick={async () => {
                  try {
                    const resp = await sendRpcCommand("ACCEPT_SCHEMATIC_CHANGES", {})
                    if (resp?.status === "OK") {
                      setPendingChanges(null)
                      addDebugLog("log", "✅ Changes accepted")
                    }
                  } catch (e: any) {
                    addDebugLog("error", `Failed to accept changes: ${e?.message}`)
                  }
                }}
                variant="outline"
                className="h-9 px-3 border-green-600 bg-green-600/10 text-green-400 hover:bg-green-600/20"
                title={`Apply changes (${pendingChanges.added} added, ${pendingChanges.removed} removed)`}
              >
                <CheckCircle2 className="h-3.5 w-3.5 mr-1" />
                Apply
              </Button>
              <Button
                onClick={async () => {
                  try {
                    const resp = await sendRpcCommand("REJECT_SCHEMATIC_CHANGES", {})
                    if (resp?.status === "OK") {
                      setPendingChanges(null)
                      addDebugLog("log", "❌ Changes rejected")
                    }
                  } catch (e: any) {
                    addDebugLog("error", `Failed to reject changes: ${e?.message}`)
                  }
                }}
                variant="outline"
                className="h-9 px-3 border-red-600 bg-red-600/10 text-red-400 hover:bg-red-600/20"
                title="Discard changes"
              >
                <XCircle className="h-3.5 w-3.5 mr-1" />
                Discard
              </Button>
            </>
          )}
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

      {/* Debug Console */}
      <div className="border-t border-[#22272F]">
        <button
          onClick={() => setShowDebugConsole(!showDebugConsole)}
          className="w-full px-3 py-1.5 text-xs text-[#9AA3AD] hover:text-[#E7E9EC] hover:bg-[#151A21] flex items-center justify-between"
        >
          <span>Debug Console ({debugLogs.length} logs)</span>
          <span>{showDebugConsole ? "▼" : "▶"}</span>
        </button>
        {showDebugConsole && (
          <div className="bg-[#0A0C0F] border-t border-[#22272F] max-h-[200px] overflow-y-auto">
            <ScrollArea className="h-full">
              <div className="p-2 space-y-0.5 font-mono text-[10px]">
                {debugLogs.length === 0 ? (
                  <div className="text-[#7C8590] px-2 py-1">No logs yet...</div>
                ) : (
                  debugLogs.map((log, idx) => (
                    <div
                      key={idx}
                      className={`px-2 py-0.5 ${
                        log.level === "error"
                          ? "text-[#FF6B6B]"
                          : log.level === "warn"
                          ? "text-[#FFD93D]"
                          : "text-[#7C8590]"
                      }`}
                    >
                      <span className="text-[#5A6268]">[{log.time}]</span>{" "}
                      <span className="text-[#5A6268]">[{log.level}]</span> {log.message}
                    </div>
                  ))
                )}
              </div>
            </ScrollArea>
          </div>
        )}
      </div>
    </div>
  )
}
