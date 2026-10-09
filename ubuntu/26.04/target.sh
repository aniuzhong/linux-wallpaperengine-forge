GUARD_PRETTY="Ubuntu 26.04"
GUARD_ID_LIKE=debian

export CC="${CC:-gcc-15}"
export CXX="${CXX:-g++-15}"

ENGINE_PATCH_DIRS=("$TARGET_DIR/patches/engine")
GUI_PATCH_DIRS=()

INTEGRATION_STAGE=1
INTEGRATION_BUILD="$TARGET_DIR/build-extension.sh"
SRC_DIR="$TARGET_DIR/src"
