add_library(kuzu_processor_operator_scan
        OBJECT
        primary_key_scan_node_table.cpp
        scan_multi_rel_tables.cpp
        scan_node_table.cpp
        scan_rel_table.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:kuzu_processor_operator_scan>
        PARENT_SCOPE)
