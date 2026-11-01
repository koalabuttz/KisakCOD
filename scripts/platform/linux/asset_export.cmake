# Offline export of user-owned font metrics using the real database loader.
# No additional target or startup hook is built unless explicitly requested.
option(KISAK_BUILD_UI_ASSET_EXPORT "Build offline local font asset exporter" OFF)
if(KISAK_BUILD_UI_ASSET_EXPORT)
    get_target_property(export_sources KisakCOD-dedi SOURCES)
    add_executable(KisakCOD-ui-asset-export ${export_sources}
        "${SRC_DIR}/linux/tools/font_asset_export.cpp")
    foreach(property COMPILE_FEATURES COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_OPTIONS)
        get_target_property(export_property KisakCOD-dedi ${property})
        if(export_property)
            set_property(TARGET KisakCOD-ui-asset-export PROPERTY ${property} "${export_property}")
        endif()
    endforeach()
    get_target_property(export_definitions KisakCOD-dedi COMPILE_DEFINITIONS)
    list(FILTER export_definitions EXCLUDE REGEX "^TRACY_")
    target_compile_definitions(KisakCOD-ui-asset-export PRIVATE ${export_definitions} KISAK_UI_ASSET_EXPORT_ONLY)
    target_include_directories(KisakCOD-ui-asset-export PRIVATE "${tracy_SOURCE_DIR}/public")
    # Profiling remains disabled: the offline tool must not start Tracy sockets.
    target_link_libraries(KisakCOD-ui-asset-export PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_dependencies(KisakCOD-ui-asset-export update_build_number)
endif()
