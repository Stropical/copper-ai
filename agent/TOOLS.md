# Schematic Agent Tools Specification

This document defines the exact specification for all tools available to the schematic agent. Tools are invoked using the format:

```
TOOL <tool_name> <json_object>
```

Where `<tool_name>` is the tool identifier and `<json_object>` is a valid JSON object containing the tool parameters.

---

## Table of Contents

1. [Component Tools](#component-tools)
2. [Connection Tools](#connection-tools)
3. [Text and Annotation Tools](#text-and-annotation-tools)
4. [Selection and Query Tools](#selection-and-query-tools)
5. [Editing Tools](#editing-tools)
6. [Validation Tools](#validation-tools)
7. [Library and Symbol Tools](#library-and-symbol-tools)
8. [Drawing Tools](#drawing-tools)
9. [Schematic Analysis Tools](#schematic-analysis-tools)
10. [Datasheet Tools](#datasheet-tools)

---

## Component Tools

### `schematic.place_component`

Places a schematic symbol (component) from the KiCad symbol library onto the schematic at the specified coordinates.

**JSON Parameters:**
```json
{
  "symbol": string (REQUIRED),
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "reference": string (OPTIONAL),
  "unit": number (OPTIONAL, default: 1),
  "rotation": number (OPTIONAL, default: 0),
  "body_style": number (OPTIONAL, default: 1)
}
```

**Parameter Details:**
- `symbol`: Library symbol identifier in format `"LibraryName:SymbolName"` (e.g., `"Device:R"`, `"power:+5V"`)
- `x`: X coordinate in millimeters
- `y`: Y coordinate in millimeters
- `reference`: Reference designator (e.g., `"R1"`, `"C2"`, `"U3"`). If not provided, KiCad auto-assigns
- `unit`: Unit number for multi-unit parts (1, 2, 3, 4, etc.)
- `rotation`: Rotation angle in degrees. Valid values: 0, 90, 180, 270
- `body_style`: Body style variant (1, 2, etc.)

**Example:**
```json
TOOL schematic.place_component {"symbol":"Device:R","x":100.0,"y":50.0,"reference":"R1"}
```

**Error Handling:**
- Returns error if symbol library or symbol name is invalid
- Returns error if coordinates are not numbers
- Returns error if rotation is not 0, 90, 180, or 270

---

### `schematic.move_component`

Moves a component to a new position.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "x": number (REQUIRED),
  "y": number (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Reference designator of component to move (e.g., `"R1"`, `"U3"`)
- `x`: New X coordinate in millimeters
- `y`: New Y coordinate in millimeters

**Example:**
```json
TOOL schematic.move_component {"reference":"R1","x":150.0,"y":75.0}
```

**Error Handling:**
- Returns error if component with given reference is not found
- Returns error if coordinates are not numbers

---

### `schematic.rotate_component`

Rotates a component by the specified angle.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "angle": number (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Reference designator of component to rotate
- `angle`: Rotation angle in degrees. Valid values: 90, 180, 270 (relative rotation) or absolute: 0, 90, 180, 270

**Example:**
```json
TOOL schematic.rotate_component {"reference":"R1","angle":90}
```

**Error Handling:**
- Returns error if component is not found
- Returns error if angle is not a valid rotation value

---

### `schematic.mirror_component`

Mirrors a component horizontally or vertically.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "axis": string (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Reference designator of component to mirror
- `axis`: Mirror axis. Valid values: `"horizontal"` or `"vertical"`

**Example:**
```json
TOOL schematic.mirror_component {"reference":"R1","axis":"horizontal"}
```

**Error Handling:**
- Returns error if component is not found
- Returns error if axis is not "horizontal" or "vertical"

---

### `schematic.delete_component`

Deletes a component from the schematic.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Reference designator of component to delete

**Example:**
```json
TOOL schematic.delete_component {"reference":"R1"}
```

**Error Handling:**
- Returns error if component is not found
- May warn if component has connections

---

### `schematic.edit_component_field`

Edits a field value of a component (reference, value, footprint, or custom field).

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "field": string (REQUIRED),
  "value": string (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Reference designator of component
- `field`: Field name. Common values: `"Reference"`, `"Value"`, `"Footprint"`, or custom field name
- `value`: New field value

**Example:**
```json
TOOL schematic.edit_component_field {"reference":"R1","field":"Value","value":"10k"}
```

**Error Handling:**
- Returns error if component is not found
- Returns error if field name is invalid

---

### `schematic.set_component_attribute`

Sets component attributes (DNP, Exclude from BOM, Exclude from Simulation, Exclude from Board).

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "attribute": string (REQUIRED),
  "value": boolean (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Reference designator of component
- `attribute`: Attribute name. Valid values: `"dnp"`, `"exclude_from_bom"`, `"exclude_from_simulation"`, `"exclude_from_board"`
- `value`: Boolean value to set

**Example:**
```json
TOOL schematic.set_component_attribute {"reference":"R1","attribute":"dnp","value":true}
```

**Error Handling:**
- Returns error if component is not found
- Returns error if attribute name is invalid

---

## Connection Tools

### `schematic.add_wire`

Adds a wire segment between two points.

**JSON Parameters:**
```json
{
  "x1": number (REQUIRED),
  "y1": number (REQUIRED),
  "x2": number (REQUIRED),
  "y2": number (REQUIRED)
}
```

**Parameter Details:**
- `x1`, `y1`: Start point coordinates in millimeters
- `x2`, `y2`: End point coordinates in millimeters

**Example:**
```json
TOOL schematic.add_wire {"x1":100.0,"y1":50.0,"x2":150.0,"y2":50.0}
```

**Note:** This is equivalent to the COMMAND `WIRE x1 y1 x2 y2` but provided as a tool for consistency.

---

### `schematic.add_junction`

Adds a junction (connection point) at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED)
}
```

**Parameter Details:**
- `x`, `y`: Junction position coordinates in millimeters

**Example:**
```json
TOOL schematic.add_junction {"x":125.0,"y":50.0}
```

**Note:** This is equivalent to the COMMAND `JUNCTION x y` but provided as a tool for consistency.

---

### `schematic.add_label`

Adds a net label at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "text": string (REQUIRED),
  "type": string (OPTIONAL)
}
```

**Parameter Details:**
- `x`, `y`: Label position coordinates in millimeters
- `text`: Label text (net name)
- `type`: Label type. Valid values: `"local"` (default), `"global"`, `"hierarchical"`, `"class"`

**Example:**
```json
TOOL schematic.add_label {"x":125.0,"y":45.0,"text":"SIGNAL_IN","type":"local"}
```

**Note:** This is equivalent to the COMMAND `LABEL x y "text"` but provided as a tool for consistency.

---

### `schematic.delete_wire`

Deletes a wire segment. The wire is identified by its start and end points (with tolerance).

**JSON Parameters:**
```json
{
  "x1": number (REQUIRED),
  "y1": number (REQUIRED),
  "x2": number (REQUIRED),
  "y2": number (REQUIRED),
  "tolerance": number (OPTIONAL, default: 0.1)
}
```

**Parameter Details:**
- `x1`, `y1`: Start point coordinates in millimeters
- `x2`, `y2`: End point coordinates in millimeters
- `tolerance`: Position tolerance in millimeters for matching wires

**Example:**
```json
TOOL schematic.delete_wire {"x1":100.0,"y1":50.0,"x2":150.0,"y2":50.0}
```

**Error Handling:**
- Returns error if no matching wire is found
- May return error if multiple wires match (ambiguous)

---

### `schematic.delete_junction`

Deletes a junction at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "tolerance": number (OPTIONAL, default: 0.1)
}
```

**Parameter Details:**
- `x`, `y`: Junction position coordinates in millimeters
- `tolerance`: Position tolerance in millimeters for matching junctions

**Example:**
```json
TOOL schematic.delete_junction {"x":125.0,"y":50.0}
```

**Error Handling:**
- Returns error if no matching junction is found

---

### `schematic.delete_label`

Deletes a label at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "tolerance": number (OPTIONAL, default: 0.1)
}
```

**Parameter Details:**
- `x`, `y`: Label position coordinates in millimeters
- `tolerance`: Position tolerance in millimeters for matching labels

**Example:**
```json
TOOL schematic.delete_label {"x":125.0,"y":45.0}
```

**Error Handling:**
- Returns error if no matching label is found

---

## Text and Annotation Tools

### `schematic.add_text`

Adds a text annotation (non-net text) at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "text": string (REQUIRED),
  "size": number (OPTIONAL, default: 1.27),
  "orientation": number (OPTIONAL, default: 0)
}
```

**Parameter Details:**
- `x`, `y`: Text position coordinates in millimeters
- `text`: Text content
- `size`: Text size in millimeters (default: 1.27mm)
- `orientation`: Text orientation in degrees (0, 90, 180, 270)

**Example:**
```json
TOOL schematic.add_text {"x":50.0,"y":25.0,"text":"Note: This is a test circuit","size":1.5}
```

**Note:** This is equivalent to the COMMAND `TEXT x y "text"` but provided as a tool for consistency.

---

### `schematic.delete_text`

Deletes a text annotation at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "tolerance": number (OPTIONAL, default: 0.1)
}
```

**Parameter Details:**
- `x`, `y`: Text position coordinates in millimeters
- `tolerance`: Position tolerance in millimeters for matching text

**Example:**
```json
TOOL schematic.delete_text {"x":50.0,"y":25.0}
```

**Error Handling:**
- Returns error if no matching text is found

---

### `schematic.edit_text`

Edits the content of a text annotation.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "text": string (REQUIRED),
  "tolerance": number (OPTIONAL, default: 0.1)
}
```

**Parameter Details:**
- `x`, `y`: Text position coordinates in millimeters
- `text`: New text content
- `tolerance`: Position tolerance in millimeters for matching text

**Example:**
```json
TOOL schematic.edit_text {"x":50.0,"y":25.0,"text":"Updated note text"}
```

**Error Handling:**
- Returns error if no matching text is found

---

## Selection and Query Tools

### `schematic.select_component`

Selects a component by reference designator.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "add_to_selection": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `reference`: Reference designator of component to select
- `add_to_selection`: If true, adds to existing selection; if false, clears selection first

**Example:**
```json
TOOL schematic.select_component {"reference":"R1"}
```

**Error Handling:**
- Returns error if component is not found

---

### `schematic.select_at_position`

Selects schematic items at the specified position.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "tolerance": number (OPTIONAL, default: 0.1),
  "add_to_selection": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `x`, `y`: Position coordinates in millimeters
- `tolerance`: Selection tolerance in millimeters
- `add_to_selection`: If true, adds to existing selection; if false, clears selection first

**Example:**
```json
TOOL schematic.select_at_position {"x":100.0,"y":50.0}
```

---

### `schematic.clear_selection`

Clears the current selection.

**JSON Parameters:**
```json
{}
```

**Example:**
```json
TOOL schematic.clear_selection {}
```

---

### `schematic.query_component`

Queries component information by reference designator.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED)
}
```

**Returns:**
```json
{
  "success": boolean,
  "component": {
    "reference": string,
    "symbol": string,
    "x": number,
    "y": number,
    "rotation": number,
    "unit": number,
    "body_style": number,
    "fields": {
      "Reference": string,
      "Value": string,
      "Footprint": string,
      ...
    },
    "pins": [
      {
        "name": string,
        "number": string,
        "x": number,
        "y": number,
        "net": string
      },
      ...
    ]
  }
}
```

**Example:**
```json
TOOL schematic.query_component {"reference":"R1"}
```

**Error Handling:**
- Returns error if component is not found

---

### `schematic.query_net`

Queries net information by net name or position.

**JSON Parameters:**
```json
{
  "net_name": string (OPTIONAL),
  "x": number (OPTIONAL),
  "y": number (OPTIONAL),
  "tolerance": number (OPTIONAL, default: 0.1)
}
```

**Note:** Either `net_name` or both `x` and `y` must be provided.

**Returns:**
```json
{
  "success": boolean,
  "net": {
    "name": string,
    "connections": [
      {
        "type": string,
        "reference": string,
        "pin": string,
        "x": number,
        "y": number
      },
      ...
    ]
  }
}
```

**Example:**
```json
TOOL schematic.query_net {"net_name":"VCC"}
```

**Error Handling:**
- Returns error if net is not found

---

### `schematic.list_components`

Lists all components in the schematic.

**JSON Parameters:**
```json
{
  "filter": object (OPTIONAL)
}
```

**Filter Options:**
```json
{
  "symbol": string (OPTIONAL),
  "reference_pattern": string (OPTIONAL),
  "field_name": string (OPTIONAL),
  "field_value": string (OPTIONAL)
}
```

**Returns:**
```json
{
  "success": boolean,
  "components": [
    {
      "reference": string,
      "symbol": string,
      "x": number,
      "y": number,
      ...
    },
    ...
  ],
  "count": number
}
```

**Example:**
```json
TOOL schematic.list_components {"filter":{"symbol":"Device:R"}}
```

---

### `schematic.list_nets`

Lists all nets in the schematic.

**JSON Parameters:**
```json
{
  "filter": object (OPTIONAL)
}
```

**Filter Options:**
```json
{
  "name_pattern": string (OPTIONAL)
}
```

**Returns:**
```json
{
  "success": boolean,
  "nets": [
    {
      "name": string,
      "connection_count": number
    },
    ...
  ],
  "count": number
}
```

**Example:**
```json
TOOL schematic.list_nets {}
```

---

## Editing Tools

### `schematic.move_selection`

Moves the currently selected items by a delta.

**JSON Parameters:**
```json
{
  "dx": number (REQUIRED),
  "dy": number (REQUIRED)
}
```

**Parameter Details:**
- `dx`: Delta X in millimeters
- `dy`: Delta Y in millimeters

**Example:**
```json
TOOL schematic.move_selection {"dx":10.0,"dy":5.0}
```

**Error Handling:**
- Returns error if no items are selected

---

### `schematic.rotate_selection`

Rotates the currently selected items.

**JSON Parameters:**
```json
{
  "angle": number (REQUIRED),
  "center_x": number (OPTIONAL),
  "center_y": number (OPTIONAL)
}
```

**Parameter Details:**
- `angle`: Rotation angle in degrees (90, 180, 270)
- `center_x`, `center_y`: Rotation center in millimeters. If not provided, uses selection center

**Example:**
```json
TOOL schematic.rotate_selection {"angle":90}
```

**Error Handling:**
- Returns error if no items are selected

---

### `schematic.mirror_selection`

Mirrors the currently selected items.

**JSON Parameters:**
```json
{
  "axis": string (REQUIRED),
  "center_x": number (OPTIONAL),
  "center_y": number (OPTIONAL)
}
```

**Parameter Details:**
- `axis`: Mirror axis. Valid values: `"horizontal"` or `"vertical"`
- `center_x`, `center_y`: Mirror center in millimeters. If not provided, uses selection center

**Example:**
```json
TOOL schematic.mirror_selection {"axis":"horizontal"}
```

**Error Handling:**
- Returns error if no items are selected

---

### `schematic.delete_selection`

Deletes the currently selected items.

**JSON Parameters:**
```json
{}
```

**Example:**
```json
TOOL schematic.delete_selection {}
```

**Error Handling:**
- Returns error if no items are selected
- May warn if items have connections

---

## Validation Tools

### `schematic.run_erc`

Runs Electrical Rule Check (ERC) on the schematic.

**JSON Parameters:**
```json
{
  "report_file": string (OPTIONAL)
}
```

**Parameter Details:**
- `report_file`: Optional path to save ERC report file

**Returns:**
```json
{
  "success": boolean,
  "errors": [
    {
      "severity": string,
      "message": string,
      "reference": string,
      "location": {
        "x": number,
        "y": number
      }
    },
    ...
  ],
  "warnings": [...],
  "error_count": number,
  "warning_count": number
}
```

**Example:**
```json
TOOL schematic.run_erc {}
```

---

### `schematic.annotate`

Annotates (assigns reference designators to) all components in the schematic.

**JSON Parameters:**
```json
{
  "mode": string (OPTIONAL, default: "full"),
  "reset": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `mode`: Annotation mode. Valid values: `"full"`, `"incremental"`, `"selected"`
- `reset`: If true, resets all reference designators before annotating

**Returns:**
```json
{
  "success": boolean,
  "annotated_count": number,
  "changes": [
    {
      "old_reference": string,
      "new_reference": string
    },
    ...
  ]
}
```

**Example:**
```json
TOOL schematic.annotate {"mode":"full","reset":false}
```

---

## Library and Symbol Tools

### `schematic.search_symbols`

Searches for symbols in the symbol libraries.

**JSON Parameters:**
```json
{
  "query": string (REQUIRED),
  "library": string (OPTIONAL),
  "limit": number (OPTIONAL, default: 20)
}
```

**Parameter Details:**
- `query`: Search query (symbol name or description)
- `library`: Optional library name to search within
- `limit`: Maximum number of results to return

**Returns:**
```json
{
  "success": boolean,
  "symbols": [
    {
      "library": string,
      "name": string,
      "description": string,
      "full_id": string
    },
    ...
  ],
  "count": number
}
```

**Example:**
```json
TOOL schematic.search_symbols {"query":"resistor","limit":10}
```

---

### `schematic.get_symbol_info`

Gets detailed information about a symbol from the library.

**JSON Parameters:**
```json
{
  "symbol": string (REQUIRED)
}
```

**Parameter Details:**
- `symbol`: Library symbol identifier in format `"LibraryName:SymbolName"`

**Returns:**
```json
{
  "success": boolean,
  "symbol": {
    "library": string,
    "name": string,
    "description": string,
    "pins": [
      {
        "name": string,
        "number": string,
        "type": string,
        "electrical_type": string
      },
      ...
    ],
    "units": number,
    "body_styles": number
  }
}
```

**Example:**
```json
TOOL schematic.get_symbol_info {"symbol":"Device:R"}
```

**Error Handling:**
- Returns error if symbol is not found

---

## Drawing Tools

### `schematic.add_rectangle`

Adds a rectangle drawing element.

**JSON Parameters:**
```json
{
  "x1": number (REQUIRED),
  "y1": number (REQUIRED),
  "x2": number (REQUIRED),
  "y2": number (REQUIRED),
  "line_width": number (OPTIONAL, default: 0.15),
  "fill": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `x1`, `y1`: Top-left corner coordinates in millimeters
- `x2`, `y2`: Bottom-right corner coordinates in millimeters
- `line_width`: Line width in millimeters
- `fill`: Whether to fill the rectangle

**Example:**
```json
TOOL schematic.add_rectangle {"x1":0.0,"y1":0.0,"x2":100.0,"y2":50.0}
```

---

### `schematic.add_circle`

Adds a circle drawing element.

**JSON Parameters:**
```json
{
  "x": number (REQUIRED),
  "y": number (REQUIRED),
  "radius": number (REQUIRED),
  "line_width": number (OPTIONAL, default: 0.15),
  "fill": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `x`, `y`: Center coordinates in millimeters
- `radius`: Circle radius in millimeters
- `line_width`: Line width in millimeters
- `fill`: Whether to fill the circle

**Example:**
```json
TOOL schematic.add_circle {"x":50.0,"y":25.0,"radius":10.0}
```

---

### `schematic.add_line`

Adds a line drawing element (non-wire, for annotations).

**JSON Parameters:**
```json
{
  "x1": number (REQUIRED),
  "y1": number (REQUIRED),
  "x2": number (REQUIRED),
  "y2": number (REQUIRED),
  "line_width": number (OPTIONAL, default: 0.15)
}
```

**Parameter Details:**
- `x1`, `y1`: Start point coordinates in millimeters
- `x2`, `y2`: End point coordinates in millimeters
- `line_width`: Line width in millimeters

**Example:**
```json
TOOL schematic.add_line {"x1":0.0,"y1":0.0,"x2":100.0,"y2":50.0}
```

---

## Schematic Analysis Tools

These tools provide comprehensive analysis and understanding of the schematic state, connections, and hierarchy.

### `schematic.get_full_state`

Gets the complete state of the schematic including all components, nets, connections, hierarchy, and relationships. This is the most comprehensive tool for understanding what's on a schematic.

**JSON Parameters:**
```json
{
  "include_hierarchy": boolean (OPTIONAL, default: true),
  "include_connections": boolean (OPTIONAL, default: true),
  "include_power": boolean (OPTIONAL, default: true),
  "include_buses": boolean (OPTIONAL, default: true),
  "sheet_path": string (OPTIONAL)
}
```

**Parameter Details:**
- `include_hierarchy`: Include hierarchical sheet information
- `include_connections`: Include detailed connection graph (pin-to-pin connections)
- `include_power`: Include power distribution analysis
- `include_buses`: Include bus information
- `sheet_path`: Optional sheet path to limit to specific sheet (default: all sheets)

**Returns:**
```json
{
  "success": boolean,
  "schematic": {
    "sheets": [
      {
        "name": string,
        "path": string,
        "page_number": number,
        "components": [...],
        "wires": [...],
        "labels": [...],
        "junctions": [...]
      },
      ...
    ],
    "hierarchy": {
      "root_sheet": string,
      "sheets": [
        {
          "name": string,
          "path": string,
          "parent": string,
          "children": [...]
        },
        ...
      ]
    },
    "connections": {
      "nets": [
        {
          "name": string,
          "connections": [
            {
              "component": string,
              "pin": string,
              "pin_name": string,
              "sheet": string
            },
            ...
          ],
          "wire_segments": [...],
          "labels": [...]
        },
        ...
      ],
      "unconnected_pins": [...]
    },
    "power_distribution": {
      "power_nets": [
        {
          "name": string,
          "voltage": string,
          "components": [...],
          "sources": [...]
        },
        ...
      ],
      "ground_nets": [...]
    },
    "statistics": {
      "total_components": number,
      "total_nets": number,
      "total_wires": number,
      "total_labels": number,
      "total_junctions": number,
      "unconnected_pins": number
    }
  }
}
```

**Example:**
```json
TOOL schematic.get_full_state {"include_hierarchy":true,"include_connections":true}
```

**Error Handling:**
- Returns error if schematic is not loaded

---

### `schematic.get_connections`

Gets the complete connection graph showing all pin-to-pin connections in the schematic.

**JSON Parameters:**
```json
{
  "net_name": string (OPTIONAL),
  "component_reference": string (OPTIONAL),
  "include_unconnected": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `net_name`: Optional net name to filter connections
- `component_reference`: Optional component reference to get connections for specific component
- `include_unconnected`: Include unconnected pins in results

**Returns:**
```json
{
  "success": boolean,
  "connections": [
    {
      "net_name": string,
      "connections": [
        {
          "from": {
            "component": string,
            "pin": string,
            "pin_name": string,
            "sheet": string
          },
          "to": {
            "component": string,
            "pin": string,
            "pin_name": string,
            "sheet": string
          },
          "via": [
            {
              "type": string,
              "x": number,
              "y": number
            },
            ...
          ]
        },
        ...
      ]
    },
    ...
  ]
}
```

**Example:**
```json
TOOL schematic.get_connections {"component_reference":"U1"}
```

**Error Handling:**
- Returns error if component is not found (when filtering by reference)

---

### `schematic.get_component_connections`

Gets all connections for a specific component, showing what each pin is connected to.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "include_unconnected": boolean (OPTIONAL, default: true)
}
```

**Parameter Details:**
- `reference`: Component reference designator
- `include_unconnected`: Include unconnected pins in results

**Returns:**
```json
{
  "success": boolean,
  "component": {
    "reference": string,
    "symbol": string,
    "pins": [
      {
        "pin_number": string,
        "pin_name": string,
        "net": string,
        "connected_to": [
          {
            "component": string,
            "pin": string,
            "pin_name": string
          },
          ...
        ],
        "is_connected": boolean
      },
      ...
    ]
  }
}
```

**Example:**
```json
TOOL schematic.get_component_connections {"reference":"U1"}
```

**Error Handling:**
- Returns error if component is not found

---

### `schematic.get_hierarchy`

Gets the hierarchical sheet structure of the schematic.

**JSON Parameters:**
```json
{
  "include_content": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `include_content`: Include component/net counts for each sheet

**Returns:**
```json
{
  "success": boolean,
  "hierarchy": {
    "root_sheet": {
      "name": string,
      "path": string,
      "uuid": string
    },
    "sheets": [
      {
        "name": string,
        "path": string,
        "uuid": string,
        "parent": string,
        "parent_path": string,
        "page_number": number,
        "file_name": string,
        "children": [...],
        "component_count": number,
        "net_count": number
      },
      ...
    ],
    "sheet_pins": [
      {
        "sheet": string,
        "pin_name": string,
        "net": string,
        "position": {
          "x": number,
          "y": number
        }
      },
      ...
    ]
  }
}
```

**Example:**
```json
TOOL schematic.get_hierarchy {"include_content":true}
```

---

### `schematic.get_power_distribution`

Gets power and ground net distribution across the schematic.

**JSON Parameters:**
```json
{
  "include_components": boolean (OPTIONAL, default: true)
}
```

**Parameter Details:**
- `include_components`: Include list of components connected to each power net

**Returns:**
```json
{
  "success": boolean,
  "power_nets": [
    {
      "name": string,
      "type": string,
      "voltage": string,
      "component_count": number,
      "components": [
        {
          "reference": string,
          "pin": string,
          "pin_name": string
        },
        ...
      ],
      "sources": [
        {
          "reference": string,
          "type": string
        },
        ...
      ]
    },
    ...
  ],
  "ground_nets": [
    {
      "name": string,
      "component_count": number,
      "components": [...]
    },
    ...
  ]
}
```

**Example:**
```json
TOOL schematic.get_power_distribution {"include_components":true}
```

---

### `schematic.get_net_topology`

Gets the topology (connection graph) of a specific net, showing all components and connections.

**JSON Parameters:**
```json
{
  "net_name": string (REQUIRED),
  "include_positions": boolean (OPTIONAL, default: true)
}
```

**Parameter Details:**
- `net_name`: Name of the net to analyze
- `include_positions`: Include position information for connections

**Returns:**
```json
{
  "success": boolean,
  "net": {
    "name": string,
    "type": string,
    "connections": [
      {
        "component": string,
        "pin": string,
        "pin_name": string,
        "position": {
          "x": number,
          "y": number
        },
        "sheet": string
      },
      ...
    ],
    "wire_segments": [
      {
        "from": {"x": number, "y": number},
        "to": {"x": number, "y": number},
        "sheet": string
      },
      ...
    ],
    "labels": [
      {
        "text": string,
        "position": {"x": number, "y": number},
        "type": string,
        "sheet": string
      },
      ...
    ],
    "junctions": [
      {
        "position": {"x": number, "y": number},
        "sheet": string
      },
      ...
    ]
  }
}
```

**Example:**
```json
TOOL schematic.get_net_topology {"net_name":"VCC"}
```

**Error Handling:**
- Returns error if net is not found

---

## Datasheet Tools

These tools provide access to component datasheet information stored in KiCad.

### `schematic.get_datasheet`

Gets datasheet information for a component. KiCad stores datasheet information in component fields, which can be URLs, file paths, or references.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "include_content": boolean (OPTIONAL, default: false)
}
```

**Parameter Details:**
- `reference`: Component reference designator
- `include_content`: If true and datasheet is a local file, attempt to include file content (for text-based datasheets)

**Returns:**
```json
{
  "success": boolean,
  "component": {
    "reference": string,
    "symbol": string,
    "datasheet": {
      "url": string,
      "type": string,
      "exists": boolean,
      "is_url": boolean,
      "is_file": boolean,
      "file_path": string,
      "content_preview": string
    },
    "description": string,
    "manufacturer": string,
    "part_number": string
  }
}
```

**Example:**
```json
TOOL schematic.get_datasheet {"reference":"U1"}
```

**Error Handling:**
- Returns error if component is not found
- Returns warning if datasheet field is empty

**Notes:**
- Datasheet field may contain:
  - HTTP/HTTPS URLs (web links)
  - File paths (relative to project or absolute)
  - Part numbers or references
- The tool will attempt to resolve file paths relative to the project directory

---

### `schematic.get_all_datasheets`

Gets datasheet information for all components in the schematic that have datasheet fields.

**JSON Parameters:**
```json
{
  "filter": object (OPTIONAL)
}
```

**Filter Options:**
```json
{
  "symbol": string (OPTIONAL),
  "has_datasheet": boolean (OPTIONAL, default: true)
}
```

**Returns:**
```json
{
  "success": boolean,
  "datasheets": [
    {
      "reference": string,
      "symbol": string,
      "datasheet": {
        "url": string,
        "type": string,
        "is_url": boolean,
        "is_file": boolean
      },
      "description": string
    },
    ...
  ],
  "count": number,
  "with_datasheets": number,
  "without_datasheets": number
}
```

**Example:**
```json
TOOL schematic.get_all_datasheets {}
```

---

### `schematic.set_datasheet`

Sets or updates the datasheet field for a component.

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED),
  "datasheet": string (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Component reference designator
- `datasheet`: Datasheet URL, file path, or reference

**Example:**
```json
TOOL schematic.set_datasheet {"reference":"U1","datasheet":"https://example.com/datasheet.pdf"}
```

**Error Handling:**
- Returns error if component is not found

---

### `schematic.open_datasheet`

Opens the datasheet for a component in the default application (browser for URLs, PDF viewer for files).

**JSON Parameters:**
```json
{
  "reference": string (REQUIRED)
}
```

**Parameter Details:**
- `reference`: Component reference designator

**Returns:**
```json
{
  "success": boolean,
  "opened": boolean,
  "datasheet_url": string,
  "message": string
}
```

**Example:**
```json
TOOL schematic.open_datasheet {"reference":"U1"}
```

**Error Handling:**
- Returns error if component is not found
- Returns error if datasheet field is empty
- Returns error if datasheet file/URL cannot be opened

**Notes:**
- For URLs, opens in default web browser
- For file paths, opens in default application for that file type
- File paths are resolved relative to the project directory

---

## Tool Response Format

All tools return a response in the following format:

**Success Response:**
```json
{
  "success": true,
  "message": string (OPTIONAL),
  "data": object (OPTIONAL, tool-specific)
}
```

**Error Response:**
```json
{
  "success": false,
  "error": string,
  "error_code": string (OPTIONAL)
}
```

---

## Coordinate System

- All coordinates are specified in **millimeters (mm)**
- Origin (0, 0) is at the top-left corner of the schematic
- X increases to the right
- Y increases downward

---

## Common Error Codes

- `COMPONENT_NOT_FOUND`: Component with given reference not found
- `SYMBOL_NOT_FOUND`: Symbol not found in library
- `INVALID_COORDINATES`: Invalid coordinate values
- `INVALID_PARAMETER`: Invalid parameter value
- `NO_SELECTION`: No items currently selected
- `OPERATION_FAILED`: General operation failure
- `NET_NOT_FOUND`: Net not found
- `ITEM_NOT_FOUND`: Schematic item not found at specified position

---

## Implementation Notes

1. **Tool Call Format**: Tools must be called using the exact format: `TOOL <tool_name> <json_object>` on a single line
2. **JSON Formatting**: JSON objects must be valid JSON with double quotes for strings
3. **Batch Operations**: Multiple tool calls can be made in sequence; they will be executed in order
4. **Undo/Redo**: All tool operations support undo/redo through KiCad's commit system
5. **Validation**: Tools should validate all parameters before execution
6. **Error Handling**: Tools should return clear error messages when operations fail

---

## Future Extensions

This specification may be extended with additional tools as needed:
- Bus tools (bus creation, bus entry management)
- Advanced drawing tools (arcs, bezier curves, polygons)
- Design rule configuration tools
- Import/export tools (netlist, BOM, etc.)
- Simulation integration tools
- Cross-probe tools (schematic to PCB)
- Design variant management tools
- Symbol editor integration tools

---

## Summary: What an LLM Needs to Fully Understand a Schematic

To fully understand what's on a schematic and what's going on, an LLM should use these tools in combination:

### Essential Understanding Tools:
1. **`schematic.get_full_state`** - Complete schematic overview (components, nets, hierarchy, connections)
2. **`schematic.get_connections`** - Detailed connection graph (pin-to-pin connections)
3. **`schematic.get_hierarchy`** - Sheet structure for hierarchical designs
4. **`schematic.get_power_distribution`** - Power and ground analysis

### Component-Specific Understanding:
5. **`schematic.query_component`** - Individual component details
6. **`schematic.get_component_connections`** - What a component is connected to
7. **`schematic.get_datasheet`** - Component datasheet information

### Net-Specific Understanding:
8. **`schematic.query_net`** - Individual net information
9. **`schematic.get_net_topology`** - Complete net connection graph

### Validation:
10. **`schematic.run_erc`** - Electrical rule check results

### Recommended Workflow for LLM Understanding:

1. **Initial Overview**: Use `schematic.get_full_state` to get complete schematic context
2. **Component Analysis**: For each component of interest, use `schematic.query_component` and `schematic.get_component_connections`
3. **Datasheet Lookup**: Use `schematic.get_datasheet` to understand component capabilities
4. **Connection Analysis**: Use `schematic.get_connections` or `schematic.get_net_topology` to understand signal flow
5. **Power Analysis**: Use `schematic.get_power_distribution` to understand power delivery
6. **Validation**: Use `schematic.run_erc` to check for design issues

### What These Tools Provide:

✅ **Complete component inventory** - All components with positions, values, footprints  
✅ **Full connection graph** - Pin-to-pin connections, net topology  
✅ **Hierarchical structure** - Sheet organization for complex designs  
✅ **Power distribution** - Power and ground nets, sources, loads  
✅ **Component datasheets** - Access to component documentation  
✅ **Net analysis** - Signal flow, connections, labels  
✅ **Design validation** - ERC results, unconnected pins  

### What's Still Missing (Future Enhancements):

- **Bus analysis tools** - Bus vector analysis, bus entry management
- **Signal flow analysis** - Automatic signal path tracing
- **Component grouping** - Functional block identification
- **Design intent inference** - Circuit topology understanding
- **Simulation data** - SPICE netlist, simulation results
- **PCB cross-reference** - Footprint assignments, layout constraints

