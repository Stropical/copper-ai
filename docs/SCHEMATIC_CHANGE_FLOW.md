## Copper AI → KiCad Schematic Change Pipeline

The Copper AI tooling applies schematic edits by generating either full `.kicad_sch` files or minimal unified diff patches. KiCad ingests those edits through the agent panel’s RPC bridge so that every data change flows through `SCH_COMMIT`, ensuring the undo stack, connectivity, highlighting, and UI stay consistent.

### Architecture Diagram

```mermaid
flowchart LR
    subgraph Cloud Agent
        IDE["Runner / IDE plug‑in"] -->|prompt & plan| AgentServer["Copper Agent Server"]
        AgentServer -->|full file or diff patch| KiCadBridge["KiCad RPC Bridge"]
    end

    subgraph KiCad Desktop
        KiCadBridge -->|JSON RPC| Frame["SCH_EDIT_FRAME RPC handler"]
        Frame -->|APPLY_SCHEMATIC_DIFF / REPLACE_SCHEMATIC| TempFile["Temp .kicad_sch Copy"]
        TempFile -->|ReplaceSchematicInRAM| Commit["SCH_COMMIT"]
        Commit -->|Push()| Canvas["Canvas + Connectivity\n(update, highlight, undo)"]
    end

    Canvas -->|status| KiCadBridge
    KiCadBridge -->|logs / pending changes| AgentServer
```

### Detailed Flow

1. **Planner / Runner**
   - The Copper AI runner (CLI, VS Code, or hosted flow) gathers context and generates either a complete schematic file (`*** Begin Full File ***`) or a unified diff patch (`*** Begin Patch ***`).
   - Results stream back to the **Agent Server**, which validates placements (e.g., guard against Y=0 corruption) and sends either:
     - `schematic_file`: absolute path to a staged `.kicad_sch`, or
     - `diff`: unified diff text to minimize churn.

2. **RPC Bridge (Agent Panel)**
   - The agent panel web UI relays commands via `window.kiclient.postMessage` into `SCH_EDIT_FRAME`.
   - Supported commands include:
     - `REPLACE_SCHEMATIC_FROM_DATA` – upload base64-encoded files.
     - `REPLACE_SCHEMATIC` – load a file already on disk.
     - `APPLY_SCHEMATIC_DIFF` *(added now)* – apply a diff without rewriting everything.

3. **SCH_EDIT_FRAME handlers**
   - `APPLY_SCHEMATIC_DIFF` creates a temporary copy of the currently open schematic, calls the unified diff parser, and writes the patched content back to the temp file.
   - Both `APPLY_SCHEMATIC_DIFF` and `REPLACE_SCHEMATIC*` converge on `ReplaceSchematicInRAM()` so the same commit/highlight logic is reused.

4. **ReplaceSchematicInRAM + SCH_COMMIT**
   - Existing items are staged for removal while new/modified items are cloned from the patched file.
   - `SCH_COMMIT::Push()` performs:
     - Undo/redo bookkeeping.
     - Connectivity graph rebuild (`RecalculateConnections`).
     - Hierarchy refresh + navigation updates.
     - Highlighting of added items + pending change counters.

5. **UI & Feedback**
   - The canvas refreshes immediately, and the agent panel queries `GET_PENDING_CHANGES` so the user can accept or reject.
   - Any error bubbles back through the RPC response (e.g., diff parse failure, file IO problems) and is displayed in the panel and runner logs.

### Why Diff Support Matters

- **Bandwidth**: Remote runners can send a few kilobytes instead of full schematics.
- **Determinism**: Diff validation ensures that patches match the current sheet, preventing accidental overwrites when the designer has unsaved edits.
- **Single Source of Truth**: By reusing `ReplaceSchematicInRAM` and `SCH_COMMIT`, the diff path inherits highlighting, undo, and OnModify behaviors automatically.

Use this document as a quick reference when extending the RPC surface or debugging the end-to-end flow for schematic automation.
