idf_component_register(
    SRCS "console_cmd.c"
    INCLUDE_DIRS "."
    REQUIRES storage_manager uploader
    PRIV_REQUIRES console
)
