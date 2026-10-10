GUARD_PRETTY="Ubuntu 26.04"
GUARD_ID_LIKE=debian

export CC="${CC:-gcc-15}"
export CXX="${CXX:-g++-15}"

ENGINE_PATCH_LIST="$TARGET_DIR/patches/engine.list"
GUI_PATCH_LIST="$TARGET_DIR/patches/gui.list"

INTEGRATION_STAGE=1
INTEGRATION_BUILD="$TARGET_DIR/build-extension.sh"
SRC_DIR="$TARGET_DIR/src"
