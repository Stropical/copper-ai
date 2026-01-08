# Using SCH_COMMIT to Replace a Schematic in RAM

## Overview

`SCH_COMMIT` is the mechanism for making changes to a schematic that are tracked in the undo/redo system and properly update the UI, connectivity graph, and other subsystems. To replace an entire schematic file in RAM, you need to:

1. Load the new schematic file
2. Remove all existing items from the current screen
3. Add all new items from the loaded schematic
4. Push the commit to apply changes

## Key Classes and Methods

### SCH_COMMIT

Located in `eeschema/sch_commit.h` and `eeschema/sch_commit.cpp`.

**Key Methods:**
- `Add(EDA_ITEM* aItem, BASE_SCREEN* aScreen)` - Stage an item to be added
- `Remove(EDA_ITEM* aItem, BASE_SCREEN* aScreen)` - Stage an item to be removed
- `Modify(EDA_ITEM* aItem, BASE_SCREEN* aScreen)` - Stage an item to be modified
- `Push(const wxString& aMessage, int aCommitFlags)` - Apply all staged changes

**Constructor:**
```cpp
SCH_COMMIT( TOOL_MANAGER* aToolMgr );
SCH_COMMIT( EDA_DRAW_FRAME* aFrame );
```

### SCH_SCREEN

Located in `eeschema/sch_screen.h` and `eeschema/sch_screen.cpp`.

**Key Methods:**
- `Items()` - Returns `EE_RTREE&` containing all items on the screen
- `Remove(SCH_ITEM* aItem, bool aUpdateLibSymbol)` - Remove an item from the screen
- `Append(SCH_ITEM* aItem, bool aUpdateLibSymbol)` - Add an item to the screen
- `Clear(bool aFree)` - Clear all items (use `aFree=true` to delete them)

## Simple Example: Basic Usage

The simplest way to use SCH_COMMIT:

```cpp
#include <sch_commit.h>
#include <sch_edit_frame.h>

void SimpleExample( SCH_EDIT_FRAME* frame )
{
    SCH_SCREEN* screen = frame->GetScreen();
    SCH_COMMIT commit( frame );
    
    // Stage changes
    commit.Add( newItem, screen );      // Add a new item
    commit.Remove( oldItem, screen );    // Remove an existing item
    commit.Modify( changedItem, screen ); // Modify an existing item
    
    // Apply all changes at once
    commit.Push( _( "My changes" ) );
    
    // After Push(), the commit is empty and all changes are applied
    // The UI is updated, undo/redo is set up, connectivity is recalculated
}
```

## Example: Replacing a Schematic

Here's a complete example of how to replace an entire schematic:

```cpp
#include <sch_commit.h>
#include <sch_screen.h>
#include <sch_io_mgr.h>
#include <schematic.h>

void ReplaceSchematicInRAM( SCH_EDIT_FRAME* frame, const wxString& newSchematicPath )
{
    // Get the current screen and schematic
    SCH_SCREEN* currentScreen = frame->GetScreen();
    SCHEMATIC* schematic = &frame->Schematic();
    
    // Create a commit object
    SCH_COMMIT commit( frame );
    
    // Step 1: Remove all existing items from the current screen
    std::vector<SCH_ITEM*> itemsToRemove;
    for( SCH_ITEM* item : currentScreen->Items() )
    {
        // Skip sheet pins and fields as they're managed by their parents
        if( item->Type() != SCH_SHEET_PIN_T && item->Type() != SCH_FIELD_T )
        {
            itemsToRemove.push_back( item );
        }
    }
    
    // Stage all removals
    for( SCH_ITEM* item : itemsToRemove )
    {
        commit.Remove( item, currentScreen );
    }
    
    // Step 2: Load the new schematic file
    SCH_IO_MGR::SCH_FILE_T fileType = SCH_IO_MGR::GuessPluginTypeFromSchPath( 
        newSchematicPath, KICTL_KICAD_ONLY );
    
    IO_RELEASER<SCH_IO> io( SCH_IO_MGR::FindPlugin( fileType ) );
    
    // Create a temporary schematic to load into
    SCHEMATIC tempSchematic( &schematic->Project() );
    tempSchematic.CreateDefaultScreens();
    
    // Load the new schematic file
    SCH_SHEET* newRootSheet = io->LoadSchematicFile( 
        newSchematicPath, &tempSchematic );
    
    if( !newRootSheet )
    {
        // Handle error
        return;
    }
    
    tempSchematic.SetTopLevelSheets( { newRootSheet } );
    
    // Step 3: Get all items from the new schematic's root screen
    SCH_SCREEN* newScreen = newRootSheet->GetScreen();
    std::vector<SCH_ITEM*> itemsToAdd;
    
    for( SCH_ITEM* item : newScreen->Items() )
    {
        // Skip sheet pins and fields as they're managed by their parents
        if( item->Type() != SCH_SHEET_PIN_T && item->Type() != SCH_FIELD_T )
        {
            // Clone the item so we own it
            SCH_ITEM* clonedItem = static_cast<SCH_ITEM*>( item->Clone() );
            itemsToAdd.push_back( clonedItem );
        }
    }
    
    // Step 4: Stage all additions
    for( SCH_ITEM* item : itemsToAdd )
    {
        commit.Add( item, currentScreen );
    }
    
    // Step 5: Push the commit to apply all changes
    // This will:
    // - Update the undo/redo system
    // - Update the connectivity graph
    // - Refresh the UI
    // - Update the view
    commit.Push( _( "Replace Schematic" ) );
    
    // Step 6: Update the schematic hierarchy and connectivity
    schematic->RefreshHierarchy();
    frame->RecalculateConnections( nullptr, GLOBAL_CLEANUP );
    
    // Refresh the canvas
    frame->GetCanvas()->Refresh();
}
```

## What Happens When You Call Push()

When you call `commit.Push()`, the following happens automatically:

1. **Undo/Redo System**: Creates undo entries for all staged changes
2. **Screen Updates**: Items are added/removed/modified in the SCH_SCREEN
3. **View Updates**: The graphics view is updated to show/hide items
4. **Connectivity Graph**: If connectable items changed, connectivity is recalculated
5. **Hierarchy**: If sheets changed, the hierarchy is refreshed
6. **UI Events**: Posts events to update the UI (selection, highlighting, etc.)
7. **Dirty Flag**: Marks the file as modified (unless SKIP_SET_DIRTY is used)
8. **Canvas Refresh**: Refreshes the canvas to show changes

All of this happens in `SCH_COMMIT::pushSchEdit()` - you don't need to do it manually!

## Important Notes

### 1. Item Ownership
- When you call `commit.Add()`, the commit takes ownership of the item
- When you call `commit.Remove()`, the commit creates a copy for undo purposes
- Don't delete items that have been added to a commit until after `Push()`
- After `Push()`, the commit is cleared and you can safely delete any temporary items

### 2. Sheet Pins and Fields
- `SCH_SHEET_PIN_T` and `SCH_FIELD_T` items are managed by their parent items
- Don't manually add/remove them - they're handled automatically

### 3. Connectivity Updates
- After replacing items, you need to recalculate connectivity:
  ```cpp
  frame->RecalculateConnections( nullptr, GLOBAL_CLEANUP );
  ```

### 4. Hierarchy Updates
- If you're replacing sheets, update the hierarchy:
  ```cpp
  schematic->RefreshHierarchy();
  ```

### 5. Commit Flags
You can use flags when calling `Push()`:
- `SKIP_UNDO` - Don't create an undo entry
- `SKIP_SET_DIRTY` - Don't mark the file as modified

Example:
```cpp
commit.Push( _( "Replace Schematic" ), SKIP_UNDO );
```

## Alternative: Using the API Handler Pattern

Looking at `eeschema/api/api_handler_sch.cpp`, there's a pattern for batch operations:

```cpp
COMMIT* commit = getCurrentCommit( clientName );

// Stage multiple changes
for( SCH_ITEM* item : itemsToRemove )
    commit->Remove( item, screen );

for( SCH_ITEM* item : itemsToAdd )
    commit->Add( item, screen );

// Push when done
pushCurrentCommit( clientName, _( "Batch update" ) );
```

## Loading a Schematic File

To load a schematic file, use the `SCH_IO` plugin system:

```cpp
SCH_IO_MGR::SCH_FILE_T fileType = SCH_IO_MGR::GuessPluginTypeFromSchPath( 
    filePath, KICTL_KICAD_ONLY );

IO_RELEASER<SCH_IO> io( SCH_IO_MGR::FindPlugin( fileType ) );

SCHEMATIC tempSchematic( project );
tempSchematic.CreateDefaultScreens();

SCH_SHEET* rootSheet = io->LoadSchematicFile( filePath, &tempSchematic );
```

## Key Implementation Details

### How pushSchEdit() Works

Looking at `sch_commit.cpp::pushSchEdit()`, here's what happens for each change type:

**CHT_ADD (Add):**
- Adds item to screen via `screen->Append()`
- Adds item to view via `view->Add()`
- Calls `frame->UpdateItem()` to update R-tree
- Adds to bulk list for `schematic->OnItemsAdded()`

**CHT_REMOVE (Remove):**
- Removes item from screen via `screen->Remove()`
- Removes item from view via `view->Remove()`
- Deselects if selected
- Adds to bulk list for `schematic->OnItemsRemoved()`

**CHT_MODIFY (Modify):**
- Calls `frame->UpdateItem()` to update R-tree
- Adds to bulk list for `schematic->OnItemsChanged()`
- Checks for connectivity changes and marks dirty if needed

After processing all items:
- Calls `schematic->OnItemsAdded/Removed/Changed()` for bulk notifications
- Calls `schematic->RefreshHierarchy()` if sheets changed
- Calls `frame->RecalculateConnections()` if connectivity is dirty
- Posts `TC_MESSAGE, TA_MODEL_CHANGE` event

### Batch Operations

You can stage many changes before calling `Push()`:

```cpp
SCH_COMMIT commit( frame );
SCH_SCREEN* screen = frame->GetScreen();

// Stage many changes
for( auto item : itemsToAdd )
    commit.Add( item, screen );

for( auto item : itemsToRemove )
    commit.Remove( item, screen );

for( auto item : itemsToModify )
    commit.Modify( item, screen );

// Apply all at once - more efficient than multiple Push() calls
commit.Push( _( "Batch update" ) );
```

## See Also

- `eeschema/sch_commit.h` - SCH_COMMIT class definition
- `eeschema/sch_commit.cpp` - SCH_COMMIT implementation (especially `pushSchEdit()`)
- `eeschema/api/api_handler_sch.cpp` - Example of using commits via API
- `eeschema/files-io.cpp` - How schematics are loaded from files
- `include/commit.h` - Base COMMIT class documentation
- `eeschema/sch_edit_frame.cpp` - How RecalculateConnections() works


