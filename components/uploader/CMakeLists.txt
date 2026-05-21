idf_component_register(
    SRCS "uploader.c"
    INCLUDE_DIRS "."
    REQUIRES storage_manager 
    PRIV_REQUIRES ftpClient freertos
)
