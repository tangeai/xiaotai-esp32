idf_build_get_property(generated XIAOTAI_GENERATED_DIR)
idf_build_get_property(selected_components XIAOTAI_SELECTED_COMPONENTS)
idf_component_register(SRCS "main.c" "${generated}/xiaotai_selected.c"
                       INCLUDE_DIRS "${generated}"
                       REQUIRES ${selected_components} xiaotai_board)
