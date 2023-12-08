#file(GLOB_RECURSE MENJIN_SOURCES ./*.c)

idf_component_register(
        SRC_DIRS
        "."
        "app"
        "system"
        "wifi"

        INCLUDE_DIRS
        "."
        "app"
        "system"
        "wifi"

        EMBED_TXTFILES server_root_cert.pem
)

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)