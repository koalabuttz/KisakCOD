# Offline export of user-owned font metrics using the real database loader.
# No additional target or startup hook is built unless explicitly requested.
option(KISAK_BUILD_UI_ASSET_EXPORT "Build offline local font asset exporter" OFF)
if(KISAK_BUILD_UI_ASSET_EXPORT)
    get_target_property(export_sources KisakCOD-dedi SOURCES)
    add_executable(KisakCOD-ui-asset-export ${export_sources}
        "${SRC_DIR}/linux/tools/font_asset_export.cpp"
        "${SRC_DIR}/linux/tools/world_surface_export.cpp"
        "${SRC_DIR}/linux/tools/world_image_capture.cpp")
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

    # Asset-free checks for the bounds, allowlist and declaration rules the world
    # inventory applies before it reads a zone. It compiles the header alone: no
    # engine source, no asset tree, no 0700 output directory.
    add_executable(KisakCOD-world-surface-inventory-test
        "${TESTS_DIR}/linux_asset_export/world_surface_inventory_test.cpp")
    target_compile_features(KisakCOD-world-surface-inventory-test PRIVATE cxx_std_20)
    target_include_directories(KisakCOD-world-surface-inventory-test PRIVATE "${SRC_DIR}")

    # Asset-free checks for the image capture the Load_Texture hook feeds. It compiles the
    # capture unit alone - no engine source, no asset tree, no zone - and drives it with the
    # same POD request the hook fills in, so the bounds, the filter, the duplicate rule and
    # the disabled path are asserted without loading anything.
    add_executable(KisakCOD-world-image-capture-test
        "${TESTS_DIR}/linux_asset_export/world_image_capture_test.cpp"
        "${SRC_DIR}/linux/tools/world_image_capture.cpp")
    target_compile_features(KisakCOD-world-image-capture-test PRIVATE cxx_std_20)
    target_include_directories(KisakCOD-world-image-capture-test PRIVATE "${SRC_DIR}")
endif()
