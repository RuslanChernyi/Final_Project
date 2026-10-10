# Після кожної збірки складає merged.bin = bootloader + таблиця розділів + прошивка.
# Wokwi вантажить у чіп лише образ за адресою 0x10000 — без таблиці розділів на 0x8000
# пристрій не знає про слоти ota_0/ota_1, і Update.begin() каже, що розділ не знайдено.
# Скрипт робить те саме, що команда esptool merge_bin із Заняття 15, тільки автоматично —
# merged.bin ніколи не відстає від свіжої збірки.
Import("env")


def merge_bin(source, target, env):
    build = env.subst("$BUILD_DIR")
    env.Execute(
        '"$PYTHONEXE" "$OBJCOPY" --chip esp32 merge_bin'
        f' -o "{build}/merged.bin" --flash_mode dio --flash_size 4MB'
        f' 0x1000 "{build}/bootloader.bin"'
        f' 0x8000 "{build}/partitions.bin"'
        f' 0x10000 "{build}/firmware.bin"'
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)
