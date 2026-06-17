#!/bin/bash
#
# setup-libdatachannel.sh
# 
# Optional setup script for libdatachannel integration into RMG
# 
# NOTE: As of recent updates, CMake automatically clones libdatachannel
# if it doesn't exist. This script is optional for manual setup via submodules.
#
# Works on Linux, macOS, and MSYS2 MinGW on Windows
#
# Usage: ./setup-libdatachannel.sh
#

set -e  # Exit on error

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
RMG_ROOT="${SCRIPT_DIR}"

echo "================================================"
echo "RMG libdatachannel Setup (Optional)"
echo "================================================"
echo ""
echo "Note: CMake will automatically clone libdatachannel"
echo "if it's not already present. This script sets up"
echo "a git submodule instead (optional)."
echo ""

# Check if we're in RMG directory
if [ ! -f "${RMG_ROOT}/CMakeLists.txt" ]; then
    echo "❌ Error: CMakeLists.txt not found in $(pwd)"
    echo "Please run this script from RMG root directory"
    exit 1
fi

echo "✓ RMG root directory detected: ${RMG_ROOT}"
echo ""

# Step 1: Add libdatachannel submodule
echo "Step 1: Adding libdatachannel as git submodule..."
if [ -d "${RMG_ROOT}/Source/3rdParty/libdatachannel" ]; then
    echo "   ✓ libdatachannel submodule already exists"
else
    echo "   Adding submodule..."
    cd "${RMG_ROOT}"
    git submodule add https://github.com/paullouisageneau/libdatachannel \
        Source/3rdParty/libdatachannel
    echo "   ✓ Submodule added"
fi
echo ""

# Step 2: Update submodules
echo "Step 2: Updating git submodules..."
cd "${RMG_ROOT}"
git submodule update --init --recursive
echo "   ✓ Submodules updated"
echo ""

# Step 3: Verify CMakeLists.txt integration
echo "Step 3: Verifying CMakeLists.txt integration..."
if grep -q "libdatachannel" "${RMG_ROOT}/Source/3rdParty/CMakeLists.txt"; then
    echo "   ✓ libdatachannel already integrated in 3rdParty CMakeLists.txt"
else
    echo "   ⚠ libdatachannel not found in 3rdParty CMakeLists.txt"
    echo "   This should have been added - check manually if needed"
fi
echo ""

# Step 4: Detect platform and suggest next steps
echo "Step 4: Platform Detection and Next Steps"
echo "========================================="
echo ""

if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]]; then
    PLATFORM="Windows"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macOS"
elif [[ "$OSTYPE" == "linux-gnu" ]]; then
    PLATFORM="Linux"
else
    PLATFORM="Unknown"
fi

echo "Detected platform: $PLATFORM"
echo ""

case "$PLATFORM" in
    Windows)
        echo "📋 Windows (MinGW) detected. Install dependencies with:"
        echo ""
        echo "   pacman -S mingw-w64-x86_64-openssl mingw-w64-x86_64-nlohmann-json mingw-w64-x86_64-libsrtp"
        echo ""
        echo "Then build with:"
        echo ""
        echo "   mkdir build && cd build"
        echo "   cmake .. -G 'MSYS Makefiles' -DNETPLAY=ON"
        echo "   make -j\$(nproc)"
        echo ""
        ;;
    macOS)
        echo "📋 macOS detected. Install dependencies with:"
        echo ""
        echo "   brew install openssl nlohmann-json srtp"
        echo ""
        echo "Then build with:"
        echo ""
        echo "   mkdir build && cd build"
        echo "   cmake .. -DNETPLAY=ON"
        echo "   make -j\$(sysctl -n hw.ncpu)"
        echo ""
        ;;
    Linux)
        echo "📋 Linux detected. Install dependencies with:"
        echo ""
        echo "   # Debian/Ubuntu"
        echo "   sudo apt-get install libssl-dev nlohmann-json3-dev libsrtp2-dev"
        echo ""
        echo "   # Fedora/RHEL"
        echo "   sudo dnf install openssl-devel nlohmann-json-devel libsrtp-devel"
        echo ""
        echo "   # Arch Linux"
        echo "   sudo pacman -S openssl nlohmann-json libsrtp"
        echo ""
        echo "Then build with:"
        echo ""
        echo "   mkdir build && cd build"
        echo "   cmake .. -DNETPLAY=ON"
        echo "   make -j\$(nproc)"
        echo ""
        ;;
    *)
        echo "❓ Unknown platform. See LIBDATACHANNEL_INTEGRATION.md for manual setup"
        ;;
esac

# Step 5: Verify libdatachannel directory
echo "Step 5: Verifying libdatachannel directory..."
if [ -d "${RMG_ROOT}/Source/3rdParty/libdatachannel/include" ]; then
    echo "   ✓ libdatachannel include directory found"
    HEADER_COUNT=$(find "${RMG_ROOT}/Source/3rdParty/libdatachannel/include" -name "*.hpp" -o -name "*.h" | wc -l)
    echo "   ✓ Found $HEADER_COUNT header files"
else
    echo "   ❌ libdatachannel include directory not found"
    echo "   Check that git submodule update completed successfully"
fi
echo ""

# Step 6: Ready to build
echo "================================================"
echo "✅ Setup Complete!"
echo "================================================"
echo ""
echo "Next steps:"
echo "1. Install platform-specific dependencies (see above)"
echo "2. Create build directory: mkdir build && cd build"
echo "3. Configure: cmake .. -DNETPLAY=ON"
echo "4. Build: make -j\$(nproc)"
echo ""
echo "For detailed troubleshooting, see:"
echo "  - LIBDATACHANNEL_INTEGRATION.md (general)"
echo "  - MINGW_LIBDATACHANNEL_SETUP.md (Windows/MinGW)"
echo ""
