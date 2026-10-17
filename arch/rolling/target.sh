GUARD_PRETTY="Arch Linux"

CC="${CC:-gcc}"
CXX="${CXX:-g++}"

ENGINE_PATCH_LIST="$TARGET_DIR/patches/engine.list"
GUI_PATCH_LIST="$TARGET_DIR/patches/gui.list"

INTEGRATION_BUILD="$FORGE_DIR/desktops/kde/build-plugin.sh"
