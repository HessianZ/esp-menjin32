#file(GLOB_RECURSE MENJIN_SOURCES ./*.c)

idf_component_register(
        SRC_DIRS
        "."
        "driver"
        "app"
        "system"
        "http"

        INCLUDE_DIRS
        "."
        "driver"
        "app"
        "system"
        "http"

        EMBED_TXTFILES server_root_cert.pem
)