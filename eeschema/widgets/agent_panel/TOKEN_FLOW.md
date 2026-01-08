# Authentication Token Flow

This document describes how authentication tokens flow through the CopperAI agent system.

## Token Flow Chain

```
Portal (copper.ai/token) - "Send to KiCad Agent" button
    ↓ (postMessage + HTTP POST)
Agent Panel (Next.js frontend)
    ↓ (localStorage + POST /api/auth/set-token)
Agent Server (Node.js/Bun)
    ↓ (Authorization header per request)
Agent Server → Codex Agent
    ↓ (OPENAI_API_KEY env var)
Codex Agent (CLI)
    ↓ (Authorization: Bearer <token>)
Router Server (Rust)
    ↓ (verifies with Supabase)
OpenRouter API
```

## 1. Portal → Agent Panel (Dual Mechanism)

The portal page at `https://portal.copper.ai/token` has a **"Send to KiCad Agent"** button that delivers the token via two mechanisms:

### Method A: postMessage (Immediate)
```javascript
// From portal.copper.ai/token
window.opener.postMessage({
  type: "copper_auth_token",
  token: "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  refresh_token: "..." // optional
}, "*");
```

### Method B: HTTP POST (Persistent)
```javascript
// Also from portal.copper.ai/token
fetch("http://127.0.0.1:5001/api/auth/set-token", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({
    access_token: "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
    refresh_token: "..." // optional
  })
});
```

The agent panel:
- Listens for `postMessage` events
- Receives POST to `/api/auth/set-token` endpoint
- Stores token in React state (`authToken`)
- Stores token in localStorage (`copper_auth_token`)
- Forwards token to agent server endpoint

## 2. Agent Panel → Agent Server

When making requests to `/api/generate`, the agent panel includes:

```javascript
headers: {
  "Content-Type": "application/json",
  "X-Session-ID": sessionId,
  "Authorization": `Bearer ${authToken}`  // ← Token from localStorage
}
```

## 3. Agent Server → Codex Agent

The Node.js server extracts the token from the `Authorization` header and passes it to the Codex agent via environment variables:

```typescript
const token = authKey.startsWith("Bearer ") ? authKey.substring(7) : authKey;
env["OPENAI_API_KEY"] = token;
env["OPENAI_BASE_URL"] = "http://localhost:9999";  // Router server
```

## 4. Codex Agent → Router Server

The Codex CLI uses the `OPENAI_API_KEY` environment variable as the Bearer token when making requests to the router server (via `OPENAI_BASE_URL`):

```http
POST http://localhost:9999/responses
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
Content-Type: application/json
```

## 5. Router Server → OpenRouter

The router server:
1. Verifies the token with Supabase (3s timeout)
2. Caches valid tokens for 5 minutes
3. Forwards requests to OpenRouter with its own API key

## New Endpoints

### POST /api/auth/set-token

Receives tokens from the portal's "Send to KiCad Agent" button.

**Request:**
```json
{
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",  // Alternative key
  "refresh_token": "..."  // Optional
}
```

**Response:**
```json
{
  "success": true,
  "message": "Token received. It will be used for subsequent requests."
}
```

## Debugging

### Check Token at Each Layer

**Agent Panel (Browser Console):**
```javascript
localStorage.getItem("copper_auth_token")
```

**Agent Server (Terminal):**
```
[server] /api/generate request - model: google/gemma-3-27b-it:free, has_auth: true
[server] Received auth token (length: 1234)
```

**Agent Runner (Terminal):**
```
[agent_runner] Auth token set (length: 1234)
[agent_runner] Using base_url: http://localhost:9999
```

**Router Server (Terminal):**
```
INFO request{method=POST uri=/responses}: router_server: Auth token present (length: 1234)
```

### Common Issues

1. **Token not arriving at agent panel**
   - Check portal postMessage is using correct format
   - Check `window.opener` is available (only works if portal was opened via `window.open()`)
   - Try the HTTP POST method instead (works without window.opener)

2. **401 after 3 seconds**
   - Token is invalid or expired
   - Supabase verification failed
   - Token not in Supabase JWT format

3. **Token not forwarded to router**
   - Check `OPENAI_API_KEY` is set in codex environment
   - Check codex is using `OPENAI_BASE_URL` correctly

### Testing "Send to KiCad Agent"

1. Start agent server: `cd agent_v2 && bun server.ts`
2. Start router server: `cd router_server && cargo run`
3. Open agent panel in KiCad
4. Open portal: `https://portal.copper.ai/token`
5. Click "Send to KiCad Agent"
6. Check browser console for success message
7. Try making a request - should not get 401

