gs_create_dymod(gs_gpex)

target_link_libraries(gs_gpex PRIVATE libpcie)

target_include_directories(
    gs_gpex PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/qemu-components/common/include>
    $<BUILD_INTERFACE:${pcie_model_SOURCE_DIR}>
    $<BUILD_INTERFACE:${pcie_model_SOURCE_DIR}/libpcie/src>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/systemc-components/pci/compat>
    $<BUILD_INTERFACE:${libqemu_SOURCE_DIR}/include/standard-headers>
)
