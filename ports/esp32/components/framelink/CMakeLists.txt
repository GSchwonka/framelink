# The framelink library, used straight from the repository root.
set(FL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
    SRCS "${FL_ROOT}/framelink/framelink.c"
    INCLUDE_DIRS "${FL_ROOT}/framelink")

if(CONFIG_FRAMELINK_CRC_TABLE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC FL_CRC_TABLE)
endif()
