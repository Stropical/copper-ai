#!/bin/bash
#
# KiCad Development Build and Run Script for macOS
# =================================================
# This is the ONLY script you need to build and run KiCad from source on macOS.
# It ensures you always build and run from the same directory to avoid confusion.
#
# Usage:
#   ./dev-build.sh              # Build KiCad (if needed) and run it
#   ./dev-build.sh build        # Force a full rebuild
#   ./dev-build.sh run          # Just run (skip build)
#   ./dev-build.sh clean        # Clean build directory
#   ./dev-build.sh configure    # Reconfigure CMake
#   ./dev-build.sh --debug      # Run with full debug logging (curl + Python agent)
#   ./dev-build.sh -j8          # Build with 8 parallel jobs (default is auto-detected)
#
# Based on official KiCad macOS build documentation:
# https://dev-docs.kicad.org/en/build/macos/

set -e  # Exit on any error

# Get the absolute path to the repository root
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${REPO_ROOT}/build/release"
KICAD_APP="${BUILD_DIR}/kicad/KiCad.app"
KICAD_BIN="${KICAD_APP}/Contents/MacOS/kicad"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored messages
info() {
    echo -e "${BLUE}ℹ${NC} $1"
}

success() {
    echo -e "${GREEN}✓${NC} $1"
}

warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

error() {
    echo -e "${RED}✗${NC} $1"
}

# Function to detect number of CPU cores
detect_cores() {
    if command -v sysctl &> /dev/null; then
        sysctl -n hw.ncpu
    else
        echo "4"  # Fallback
    fi
}

# Parse command line arguments
ACTION="build_and_run"
MAKE_ARGS=""
CLEAN=false
CONFIGURE=false
FORCE_BUILD=false
DEBUG_MODE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        build)
            ACTION="build"
            FORCE_BUILD=true
            shift
            ;;
        run)
            ACTION="run"
            shift
            ;;
        clean)
            CLEAN=true
            shift
            ;;
        configure)
            CONFIGURE=true
            shift
            ;;
        --debug|-d)
            DEBUG_MODE=true
            shift
            ;;
        -j*)
            MAKE_ARGS="$1"
            shift
            ;;
        *)
            error "Unknown option: $1"
            echo "Usage: $0 [build|run|clean|configure] [--debug|-d] [-jN]"
            exit 1
            ;;
    esac
done

# Set default parallelism if not specified
if [ -z "$MAKE_ARGS" ]; then
    CORES=$(detect_cores)
    MAKE_ARGS="-j${CORES}"
    info "Auto-detected ${CORES} CPU cores, using ${MAKE_ARGS}"
fi

# Display configuration
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  KiCad Development Build Script for macOS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
info "Repository root: ${REPO_ROOT}"
info "Build directory: ${BUILD_DIR}"
info "KiCad binary:    ${KICAD_BIN}"
echo ""

# Clean build directory if requested
if [ "$CLEAN" = true ]; then
    warning "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
    success "Build directory cleaned"
    exit 0
fi

# Check for required tools
if ! command -v cmake &> /dev/null; then
    error "CMake is not installed. Please install it:"
    echo "  brew install cmake"
    exit 1
fi

if ! command -v make &> /dev/null; then
    error "Make is not installed. Please install Xcode Command Line Tools:"
    echo "  xcode-select --install"
    exit 1
fi

# Create build directory if it doesn't exist
mkdir -p "${BUILD_DIR}"

# Configure CMake if needed or requested
if [ "$CONFIGURE" = true ] || [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    info "Configuring CMake..."
    cd "${BUILD_DIR}"
    
    # Auto-detect kicad-mac-builder toolchain
    if [ -z "${KICAD_MAC_BUILDER_TOOLCHAIN}" ]; then
        # Try common locations
        POSSIBLE_PATHS=(
            "${HOME}/Documents/GitHub/kicad-mac-builder/toolchain/kicad-mac-builder.cmake"
            "${REPO_ROOT}/../kicad-mac-builder/toolchain/kicad-mac-builder.cmake"
            "./kicad-mac-builder/toolchain/kicad-mac-builder.cmake"
            "../kicad-mac-builder/toolchain/kicad-mac-builder.cmake"
        )
        
        for path in "${POSSIBLE_PATHS[@]}"; do
            if [ -f "${path}" ]; then
                KICAD_MAC_BUILDER_TOOLCHAIN="${path}"
                break
            fi
        done
    fi
    
    # Extract kicad-mac-builder directory from toolchain path
    if [ -n "${KICAD_MAC_BUILDER_TOOLCHAIN}" ]; then
        KICAD_MAC_BUILDER_DIR="$(cd "$(dirname "${KICAD_MAC_BUILDER_TOOLCHAIN}")/.." && pwd)"
    fi
    
    # Check if we're using kicad-mac-builder toolchain
    if [ -n "${KICAD_MAC_BUILDER_TOOLCHAIN}" ] && [ -f "${KICAD_MAC_BUILDER_TOOLCHAIN}" ]; then
        info "Using kicad-mac-builder toolchain: ${KICAD_MAC_BUILDER_TOOLCHAIN}"
        
        # Verify ngspice is built
        NGSPICE_DEST="${KICAD_MAC_BUILDER_DIR}/build/ngspice-dest"
        if [ ! -f "${NGSPICE_DEST}/lib/libngspice.dylib" ]; then
            warning "ngspice library not found at ${NGSPICE_DEST}/lib/libngspice.dylib"
            warning "You may need to build ngspice first:"
            echo ""
            echo "  cd ${KICAD_MAC_BUILDER_DIR}"
            echo "  WX_SKIP_DOXYGEN_VERSION_CHECK=1 ./build.py --arch $(uname -m | sed 's/x86_64/x86_64/;s/arm64/arm64/') --target ngspice"
            echo ""
            read -p "Continue anyway? (y/N) " -n 1 -r
            echo
            if [[ ! $REPLY =~ ^[Yy]$ ]]; then
                exit 1
            fi
        fi
        
        # Explicitly set ngspice paths as cache variables to ensure Findngspice.cmake finds them
        NGSPICE_INCLUDE="${KICAD_MAC_BUILDER_DIR}/build/ngspice-dest/include"
        NGSPICE_LIB="${KICAD_MAC_BUILDER_DIR}/build/ngspice-dest/lib/libngspice.dylib"
        
        if [ ! -f "${NGSPICE_LIB}" ]; then
            error "ngspice library not found at: ${NGSPICE_LIB}"
            error "Please build ngspice first:"
            echo ""
            echo "  cd ${KICAD_MAC_BUILDER_DIR}"
            echo "  WX_SKIP_DOXYGEN_VERSION_CHECK=1 ./build.py --arch $(uname -m | sed 's/x86_64/x86_64/;s/arm64/arm64/') --target ngspice"
            echo ""
            exit 1
        fi
        
        # Get OCC paths from toolchain (they're set there)
        # But also pass them explicitly to ensure FindOCC.cmake finds them
        OCC_INCLUDE_VAR=""
        OCC_LIBRARY_VAR=""
        if [ -n "${KICAD_MAC_BUILDER_DIR}" ]; then
            # Try to read from toolchain or use defaults
            OCC_INCLUDE_VAR="-DOCC_INCLUDE_DIR=/opt/homebrew/Cellar/opencascade/7.9.3/include/opencascade"
            OCC_LIBRARY_VAR="-DOCC_LIBRARY_DIR=/opt/homebrew/Cellar/opencascade/7.9.3/lib"
        fi
        
        # Python paths from toolchain - set both PYTHON_LIBRARY and PYTHON_LIBRARIES
        PYTHON_LIB_VAR=""
        PYTHON_LIBRARY_VAR=""
        PYTHON_EXE_VAR=""
        PYTHON_INC_VAR=""
        PYTHON_LIB_PATH="${KICAD_MAC_BUILDER_DIR}/build/python-dest/Library/Frameworks/Python.framework/Versions/Current/lib/libpython3.9.dylib"
        if [ -f "${PYTHON_LIB_PATH}" ]; then
            PYTHON_LIB_VAR="-DPYTHON_LIBRARIES=${PYTHON_LIB_PATH}"
            PYTHON_LIBRARY_VAR="-DPYTHON_LIBRARY=${PYTHON_LIB_PATH}"
            PYTHON_EXE_VAR="-DPYTHON_EXECUTABLE=${KICAD_MAC_BUILDER_DIR}/build/python-dest/Library/Frameworks/Python.framework/Versions/Current/bin/python3"
            PYTHON_INC_VAR="-DPYTHON_INCLUDE_DIR=${KICAD_MAC_BUILDER_DIR}/build/python-dest/Library/Frameworks/Python.framework/Versions/Current/include/python3.9"
        fi
        
        cmake -G "Unix Makefiles" \
              -DCMAKE_TOOLCHAIN_FILE="${KICAD_MAC_BUILDER_TOOLCHAIN}" \
              -DCMAKE_BUILD_TYPE=Release \
              -DNGSPICE_INCLUDE_DIR="${NGSPICE_INCLUDE}" \
              -DNGSPICE_LIBRARY="${NGSPICE_LIB}" \
              -DNGSPICE_DLL="${NGSPICE_LIB}" \
              ${OCC_INCLUDE_VAR} \
              ${OCC_LIBRARY_VAR} \
              ${PYTHON_EXE_VAR} \
              ${PYTHON_INC_VAR} \
              ${PYTHON_LIB_VAR} \
              ${PYTHON_LIBRARY_VAR} \
              "${REPO_ROOT}"
    else
        # Standard CMake configuration for macOS
        # Note: This assumes dependencies are installed via Homebrew
        warning "kicad-mac-builder toolchain not found. Using standard CMake configuration."
        warning "This may fail if ngspice is not installed. For best results, use kicad-mac-builder:"
        echo ""
        echo "  export KICAD_MAC_BUILDER_TOOLCHAIN=\"/path/to/kicad-mac-builder/toolchain/kicad-mac-builder.cmake\""
        echo ""
        
        cmake -G "Unix Makefiles" \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_INSTALL_PREFIX=/usr/local \
              "${REPO_ROOT}"
    fi
    
    success "CMake configuration complete"
    echo ""
fi

# Build if needed or requested
if [ "$ACTION" = "build" ] || [ "$ACTION" = "build_and_run" ] || [ "$FORCE_BUILD" = true ]; then
    if [ "$FORCE_BUILD" = true ]; then
        info "Force building KiCad..."
    else
        info "Building KiCad (incremental build)..."
    fi
    
    cd "${BUILD_DIR}"
    
    # Build resources first (creates images.tar.gz)
    info "Building resources..."
    make ${MAKE_ARGS} bitmap_archive_build
    
    # Build schema copy targets (ensures schemas are in build directory)
    info "Building schema copy targets..."
    make ${MAKE_ARGS} schema_build_copy api_schema_build_copy 2>/dev/null || true
    
    # Build all kiface plugins (required for KiCad to run)
    info "Building kiface plugins..."
    make ${MAKE_ARGS} eeschema_kiface pcbnew_kiface gerbview_kiface cvpcb_kiface \
         pcb_calculator_kiface pl_editor_kiface scripting_kiface
    
    # Build the main KiCad application
    info "Building KiCad application..."
    make ${MAKE_ARGS} kicad
    
    # Ensure resources are in the right place for running from build directory
    RESOURCES_DIR="${BUILD_DIR}/kicad/KiCad.app/Contents/SharedSupport/resources"
    if [ ! -d "${RESOURCES_DIR}" ]; then
        mkdir -p "${RESOURCES_DIR}"
    fi
    
    # Copy images.tar.gz to app bundle if it exists and isn't already there
    if [ -f "${BUILD_DIR}/resources/images.tar.gz" ] && [ ! -f "${RESOURCES_DIR}/images.tar.gz" ]; then
        info "Copying images.tar.gz to app bundle..."
        cp "${BUILD_DIR}/resources/images.tar.gz" "${RESOURCES_DIR}/"
    fi
    
    # Ensure schema files are in the build directory for KICAD_RUN_FROM_BUILD_DIR
    # On macOS, when KICAD_RUN_FROM_BUILD_DIR is set, GetStockDataPath goes up 4 dirs
    # from executable (build/release/kicad/KiCad.app/Contents/MacOS/kicad) to get 
    # build root (build/release), so schemas go at build/release/kicad/schemas/
    SCHEMAS_DIR="${BUILD_DIR}/kicad/schemas"
    mkdir -p "${SCHEMAS_DIR}"
    
    # Copy schemas from build directory (already copied by CMake targets) to the location
    # where KiCad expects them when running from build directory
    BUILD_SCHEMAS_DIR="${BUILD_DIR}/schemas"
    if [ -d "${BUILD_SCHEMAS_DIR}" ]; then
        info "Copying schemas from ${BUILD_SCHEMAS_DIR} to ${SCHEMAS_DIR}..."
        cp -f "${BUILD_SCHEMAS_DIR}"/*.schema.json "${SCHEMAS_DIR}/" 2>/dev/null || true
    else
        # Fallback: try to copy from source tree
        API_SCHEMA_SOURCE="${REPO_ROOT}/api/schemas/api.v1.schema.json"
        PCM_SCHEMA_SOURCE="${REPO_ROOT}/kicad/pcm/schemas/pcm.v1.schema.json"
        [ -f "${API_SCHEMA_SOURCE}" ] && cp -f "${API_SCHEMA_SOURCE}" "${SCHEMAS_DIR}/" 2>/dev/null || true
        [ -f "${PCM_SCHEMA_SOURCE}" ] && cp -f "${PCM_SCHEMA_SOURCE}" "${SCHEMAS_DIR}/" 2>/dev/null || true
    fi
    
    # Ensure kicad_pyshell is in the build directory for KICAD_RUN_FROM_BUILD_DIR
    # On macOS, when KICAD_RUN_FROM_BUILD_DIR is set, GetStockDataPath goes up 4 dirs
    # from executable (build/release/kicad/KiCad.app/Contents/MacOS/kicad) to get 
    # build/release/kicad, then GetStockScriptingPath adds /scripting, so it expects
    # build/release/kicad/scripting/kicad_pyshell
    SCRIPTING_DIR="${BUILD_DIR}/kicad/scripting"
    KICAD_PYSHELL_SOURCE="${REPO_ROOT}/scripting/kicad_pyshell"
    KICAD_PYSHELL_DEST="${SCRIPTING_DIR}/kicad_pyshell"
    if [ -d "${KICAD_PYSHELL_SOURCE}" ]; then
        mkdir -p "${SCRIPTING_DIR}"
        if [ ! -d "${KICAD_PYSHELL_DEST}" ] || [ "${KICAD_PYSHELL_SOURCE}" -nt "${KICAD_PYSHELL_DEST}" ]; then
            info "Copying kicad_pyshell to ${KICAD_PYSHELL_DEST}..."
            cp -R "${KICAD_PYSHELL_SOURCE}" "${KICAD_PYSHELL_DEST}"
        fi
    fi
    
    # Install CopperAI plugin to third party plugins directory
    # KiCad searches: ~/Documents/kicad/9.99/plugins (user) and ~/Documents/kicad/9.99/3rdparty (third party/PCM)
    # Using 3rdparty/plugins directory for third-party plugins
    PLUGIN_SOURCE="${REPO_ROOT}/PRIVATE/com_copperai_copperagent"
    if [ -n "${KICAD_DOCUMENTS_HOME}" ]; then
        PLUGINS_DIR="${KICAD_DOCUMENTS_HOME}/kicad/9.99/3rdparty/plugins"
    else
        PLUGINS_DIR="${HOME}/Documents/kicad/9.99/3rdparty/plugins"
    fi
    
    if [ -d "${PLUGIN_SOURCE}" ]; then
        info "Installing CopperAI plugin..."
        info "  Plugin source: ${PLUGIN_SOURCE}"
        info "  Plugins directory: ${PLUGINS_DIR}"
        
        # Create plugins directory if it doesn't exist
        mkdir -p "${PLUGINS_DIR}"
        
        # Remove all existing plugins
        if [ -d "${PLUGINS_DIR}" ] && [ "$(ls -A "${PLUGINS_DIR}" 2>/dev/null)" ]; then
            info "Removing existing plugins from ${PLUGINS_DIR}..."
            rm -rf "${PLUGINS_DIR}"/*
            success "Existing plugins removed"
        fi
        
        # Copy CopperAI plugin
        PLUGIN_DEST="${PLUGINS_DIR}/com_copperai_copperagent"
        if [ -d "${PLUGIN_DEST}" ]; then
            info "Removing existing CopperAI plugin..."
            rm -rf "${PLUGIN_DEST}"
        fi
        
        info "Copying CopperAI plugin from ${PLUGIN_SOURCE} to ${PLUGIN_DEST}..."
        cp -R "${PLUGIN_SOURCE}" "${PLUGIN_DEST}"
        success "CopperAI plugin installed to ${PLUGIN_DEST}"
    else
        warning "CopperAI plugin source not found at: ${PLUGIN_SOURCE}"
        warning "Skipping plugin installation"
    fi
    
    if [ -f "${KICAD_BIN}" ]; then
        success "Build complete! KiCad binary is ready."
    else
        error "Build completed but KiCad binary not found at expected location:"
        error "  ${KICAD_BIN}"
        error "Trying to find it..."
        
        # Try to find the binary
        FOUND_BIN=$(find "${BUILD_DIR}" -name "kicad" -type f -perm +111 2>/dev/null | head -1)
        if [ -n "$FOUND_BIN" ]; then
            warning "Found binary at: ${FOUND_BIN}"
            KICAD_BIN="${FOUND_BIN}"
        else
            error "Could not find KiCad binary. Build may have failed."
            exit 1
        fi
    fi
    echo ""
fi

# Run KiCad if requested
if [ "$ACTION" = "run" ] || [ "$ACTION" = "build_and_run" ]; then
    if [ ! -f "${KICAD_BIN}" ]; then
        error "KiCad binary not found at: ${KICAD_BIN}"
        error "Please build first: $0 build"
        exit 1
    fi
    
    # Ensure schema files are present (in case we're just running, not building)
    SCHEMAS_DIR="${BUILD_DIR}/kicad/schemas"
    mkdir -p "${SCHEMAS_DIR}"
    BUILD_SCHEMAS_DIR="${BUILD_DIR}/schemas"
    if [ -d "${BUILD_SCHEMAS_DIR}" ]; then
        cp -f "${BUILD_SCHEMAS_DIR}"/*.schema.json "${SCHEMAS_DIR}/" 2>/dev/null || true
    fi
    
    # Ensure kicad_pyshell is present (in case we're just running, not building)
    SCRIPTING_DIR="${BUILD_DIR}/kicad/scripting"
    KICAD_PYSHELL_SOURCE="${REPO_ROOT}/scripting/kicad_pyshell"
    KICAD_PYSHELL_DEST="${SCRIPTING_DIR}/kicad_pyshell"
    if [ -d "${KICAD_PYSHELL_SOURCE}" ]; then
        mkdir -p "${SCRIPTING_DIR}"
        if [ ! -d "${KICAD_PYSHELL_DEST}" ]; then
            cp -R "${KICAD_PYSHELL_SOURCE}" "${KICAD_PYSHELL_DEST}"
        fi
    fi
    
    # Set environment variable to run from build directory
    export KICAD_RUN_FROM_BUILD_DIR=1
    
    # Configure Python environment for KiCad scripting support
    # When KICAD_USE_EXTERNAL_PYTHONHOME is set, KiCad won't override PYTHONPATH/PYTHONHOME
    # This allows us to control the Python environment explicitly
    
    # Detect kicad-mac-builder directory if not already set
    if [ -z "${KICAD_MAC_BUILDER_DIR}" ] && [ -n "${KICAD_MAC_BUILDER_TOOLCHAIN}" ]; then
        KICAD_MAC_BUILDER_DIR="$(cd "$(dirname "${KICAD_MAC_BUILDER_TOOLCHAIN}")/.." && pwd)"
    elif [ -z "${KICAD_MAC_BUILDER_DIR}" ]; then
        # Try to auto-detect from common locations
        POSSIBLE_PATHS=(
            "${HOME}/Documents/GitHub/kicad-mac-builder"
            "${REPO_ROOT}/../kicad-mac-builder"
        )
        for path in "${POSSIBLE_PATHS[@]}"; do
            if [ -f "${path}/toolchain/kicad-mac-builder.cmake" ]; then
                KICAD_MAC_BUILDER_DIR="${path}"
                break
            fi
        done
    fi
    
    # Detect Python installation
    PYTHON_HOME=""
    PYTHON_MODULE_DIR=""
    PYTHON_SCRIPTING_DIR=""
    
    # Check if using kicad-mac-builder Python
    if [ -n "${KICAD_MAC_BUILDER_DIR}" ] && [ -d "${KICAD_MAC_BUILDER_DIR}/build/python-dest" ]; then
        PYTHON_FRAMEWORK="${KICAD_MAC_BUILDER_DIR}/build/python-dest/Library/Frameworks/Python.framework/Versions/Current"
        if [ -d "${PYTHON_FRAMEWORK}" ]; then
            PYTHON_HOME="${PYTHON_FRAMEWORK}"
            info "Using kicad-mac-builder Python framework: ${PYTHON_HOME}"
        fi
        
        # Set DYLD_LIBRARY_PATH for kicad-mac-builder Python
        if [ -n "${KICAD_MAC_BUILDER_PYTHON_DEST}" ]; then
            export DYLD_LIBRARY_PATH="${KICAD_MAC_BUILDER_PYTHON_DEST}:${DYLD_LIBRARY_PATH}"
        fi
    else
        # Try to detect system Python
        if command -v python3 &> /dev/null; then
            PYTHON_EXE=$(which python3)
            # Get Python home (parent of bin directory)
            PYTHON_HOME=$(python3 -c "import sys; print(sys.prefix)" 2>/dev/null || echo "")
            if [ -n "${PYTHON_HOME}" ]; then
                info "Using system Python: ${PYTHON_HOME}"
            fi
        fi
    fi
    
    # Find pcbnew Python module directory
    # On macOS, pcbnew.py and _pcbnew.so are copied to PYTHON_DEST during build
    # But they're also in the pcbnew build directory
    # Try multiple locations in order of preference
    if [ -f "${BUILD_DIR}/pcbnew/pcbnew.py" ] || [ -f "${BUILD_DIR}/pcbnew/_pcbnew.so" ]; then
        PYTHON_MODULE_DIR="$(cd "${BUILD_DIR}/pcbnew" && pwd)"
    elif [ -f "${BUILD_DIR}/pcbnew/Release/pcbnew.py" ] || [ -f "${BUILD_DIR}/pcbnew/Release/_pcbnew.so" ]; then
        PYTHON_MODULE_DIR="$(cd "${BUILD_DIR}/pcbnew/Release" && pwd)"
    elif [ -f "${BUILD_DIR}/pcbnew/Debug/pcbnew.py" ] || [ -f "${BUILD_DIR}/pcbnew/Debug/_pcbnew.so" ]; then
        PYTHON_MODULE_DIR="$(cd "${BUILD_DIR}/pcbnew/Debug" && pwd)"
    else
        # Try to find in app bundle Python site-packages (where PYTHON_DEST points on macOS)
        # This is typically: build/release/kicad/KiCad.app/Contents/Frameworks/Python.framework/.../site-packages
        APP_BUNDLE_PYTHON_DIRS=(
            "${BUILD_DIR}/kicad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/lib/python*/site-packages"
            "${BUILD_DIR}/kicad/KiCad.app/Contents/Frameworks/Python.framework/Versions/*/lib/python*/site-packages"
        )
        for pattern in "${APP_BUNDLE_PYTHON_DIRS[@]}"; do
            for dir in ${pattern}; do
                if [ -d "${dir}" ] && ([ -f "${dir}/pcbnew.py" ] || [ -f "${dir}/_pcbnew.so" ]); then
                    PYTHON_MODULE_DIR="$(cd "${dir}" && pwd)"
                    break 2
                fi
            done
        done
    fi
    
    # Find kicad_pyshell directory
    if [ -d "${BUILD_DIR}/kicad/scripting/kicad_pyshell" ]; then
        PYTHON_SCRIPTING_DIR="$(cd "${BUILD_DIR}/kicad/scripting" && pwd)"
    elif [ -d "${REPO_ROOT}/scripting/kicad_pyshell" ]; then
        PYTHON_SCRIPTING_DIR="$(cd "${REPO_ROOT}/scripting" && pwd)"
    fi
    
    # Verify required modules exist
    if [ -z "${PYTHON_MODULE_DIR}" ]; then
        warning "pcbnew Python module directory not found - pcbnew.py and _pcbnew.so may be missing"
        warning "Searched in: ${BUILD_DIR}/pcbnew, ${BUILD_DIR}/pcbnew/Release, ${BUILD_DIR}/pcbnew/Debug, and app bundle Python site-packages"
        warning "This may cause 'ModuleNotFoundError: No module named pcbnew' errors"
    fi
    
    if [ -z "${PYTHON_SCRIPTING_DIR}" ]; then
        error "kicad_pyshell directory not found!"
        error "Expected at: ${BUILD_DIR}/kicad/scripting/kicad_pyshell"
        error "or: ${REPO_ROOT}/scripting/kicad_pyshell"
        exit 1
    fi
    
    # Set KICAD_USE_EXTERNAL_PYTHONHOME to prevent KiCad from overriding our PYTHONPATH
    export KICAD_USE_EXTERNAL_PYTHONHOME=1
    
    # Set PYTHONHOME if we found a Python installation
    if [ -n "${PYTHON_HOME}" ]; then
        export PYTHONHOME="${PYTHON_HOME}"
        info "PYTHONHOME set to: ${PYTHONHOME}"
    else
        warning "PYTHONHOME not set - KiCad may have trouble finding Python"
    fi
    
    # Build PYTHONPATH: must include both pcbnew module directory and scripting directory
    # Order matters: pcbnew directory first (for pcbnew module), then scripting (for kicad_pyshell)
    NEW_PYTHONPATH=""
    
    # Add pcbnew module directory (contains pcbnew.py and _pcbnew.so)
    if [ -n "${PYTHON_MODULE_DIR}" ]; then
        NEW_PYTHONPATH="${PYTHON_MODULE_DIR}"
    fi
    
    # Add scripting directory (contains kicad_pyshell module)
    if [ -n "${PYTHON_SCRIPTING_DIR}" ]; then
        if [ -n "${NEW_PYTHONPATH}" ]; then
            NEW_PYTHONPATH="${NEW_PYTHONPATH}:${PYTHON_SCRIPTING_DIR}"
        else
            NEW_PYTHONPATH="${PYTHON_SCRIPTING_DIR}"
        fi
    fi
    
    # Prepend to existing PYTHONPATH if it exists
    if [ -n "${PYTHONPATH}" ]; then
        export PYTHONPATH="${NEW_PYTHONPATH}:${PYTHONPATH}"
    else
        export PYTHONPATH="${NEW_PYTHONPATH}"
    fi
    
    info "PYTHONPATH set to: ${PYTHONPATH}"
    info "  - pcbnew module: ${PYTHON_MODULE_DIR:-<not found>}"
    info "  - kicad_pyshell: ${PYTHON_SCRIPTING_DIR:-<not found>}"
    
    # Enable debug logging based on DEBUG_MODE flag
    if [ "$DEBUG_MODE" = true ]; then
        # Enable debug logging for curl (KiCad's HTTP client)
        export KICAD_CURL_VERBOSE=1
        info "Curl verbose logging enabled (KICAD_CURL_VERBOSE=1)"
        
        # Enable debug logging for Python agent if it's running
        # This will be picked up by the agent when it starts
        export LOG_LEVEL=DEBUG
        info "Debug logging enabled (LOG_LEVEL=DEBUG)"
    else
        # Default: only enable curl verbose (less verbose than full debug)
        export KICAD_CURL_VERBOSE=1
        info "Curl verbose logging enabled (KICAD_CURL_VERBOSE=1)"
        info "Use --debug flag for full debug logging"
    fi
    
    # Enable API plugin tracing to debug plugin loading
    export WXTRACE="KICAD_API"
    info "API plugin tracing enabled (WXTRACE=KICAD_API)"
    
    success "Launching KiCad from build directory..."
    info "  Binary: ${KICAD_BIN}"
    info "  KICAD_RUN_FROM_BUILD_DIR=1"
    info "  KICAD_USE_EXTERNAL_PYTHONHOME=1 (using external Python environment)"
    if [ -n "${PYTHONHOME}" ]; then
        info "  PYTHONHOME=${PYTHONHOME}"
    fi
    if [ -n "${PYTHONPATH}" ]; then
        info "  PYTHONPATH=${PYTHONPATH}"
    fi
    if [ "$DEBUG_MODE" = true ]; then
        info "  KICAD_CURL_VERBOSE=1 (curl debug output to stderr)"
        info "  LOG_LEVEL=DEBUG (for Python agent)"
    else
        info "  KICAD_CURL_VERBOSE=1 (curl debug output to stderr)"
    fi
    echo ""
    
    # Launch KiCad with any remaining arguments
    exec "${KICAD_BIN}" "$@"
fi

success "Done!"

