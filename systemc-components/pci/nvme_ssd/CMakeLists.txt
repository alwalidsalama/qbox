gs_create_dymod(nvme_ssd)

target_link_libraries(nvme_ssd PRIVATE libpcie)

target_sources(
    nvme_ssd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nvme_ssd.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bitset.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/def.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/disk.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dma.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interface.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/namespace.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pci-device-base.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/queue.cc
)

target_include_directories(
    nvme_ssd PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/systemc-components/pci/gs_gpex/include>
    $<BUILD_INTERFACE:${pcie_model_SOURCE_DIR}>
    $<BUILD_INTERFACE:${pcie_model_SOURCE_DIR}/libpcie/src>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/systemc-components/pci/compat>
    $<BUILD_INTERFACE:${libqemu_SOURCE_DIR}/include/standard-headers>
)
