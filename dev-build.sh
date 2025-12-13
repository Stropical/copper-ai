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
    
    # Set environment variable to run from build directory
    export KICAD_RUN_FROM_BUILD_DIR=1
    
    # Set DYLD_LIBRARY_PATH if kicad-mac-builder is being used
    if [ -n "${KICAD_MAC_BUILDER_PYTHON_DEST}" ]; then
        export DYLD_LIBRARY_PATH="${KICAD_MAC_BUILDER_PYTHON_DEST}:${DYLD_LIBRARY_PATH}"
        info "Using kicad-mac-builder Python framework"
    fi
    
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
    
    success "Launching KiCad from build directory..."
    info "  Binary: ${KICAD_BIN}"
    info "  KICAD_RUN_FROM_BUILD_DIR=1"
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

