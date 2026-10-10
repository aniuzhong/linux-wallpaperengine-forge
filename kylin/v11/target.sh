GUARD_PRETTY="Kylin V11"
GUARD_ID_LIKE=openKylin

export CC="${CC:-gcc-13}"
export CXX="${CXX:-g++-13}"

ENGINE_PATCH_LIST="$TARGET_DIR/patches/engine.list"
GUI_PATCH_LIST="$TARGET_DIR/patches/gui.list"

GO_REPLACE_PKG=lwe-forge/pkg/background
GO_REPLACE_DIR="$TARGET_DIR/pkg/background"
