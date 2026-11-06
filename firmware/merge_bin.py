# After each build, merge bootloader + partition table + app into one image
# that flashes at offset 0x0, so it can be written from another machine with a
# browser flasher (web.esphome.io or espressif.github.io/esptool-js).
Import("env")

APP_BIN = "$BUILD_DIR/${PROGNAME}.bin"
MERGED_BIN = "$PROJECT_DIR/slink-bridge-${PIOENV}-full.bin"
board = env.BoardConfig()


def merge_bin(source, target, env):
    images = env.Flatten(env.get("FLASH_EXTRA_IMAGES", [])) + ["$ESP32_APP_OFFSET", APP_BIN]
    env.Execute(" ".join([
        '"$PYTHONEXE"', '"$OBJCOPY"', "--chip", board.get("build.mcu", "esp32"),
        "merge_bin", "-o", f'"{MERGED_BIN}"',
    ] + [f'"{i}"' for i in images]))


env.AddPostAction(APP_BIN, merge_bin)
