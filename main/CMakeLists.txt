idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
)

# Board name compiled in for logging / runtime guard
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_OPENPAW_BOARD_NAME="$ENV{OPENPAW_BOARD}"
)
