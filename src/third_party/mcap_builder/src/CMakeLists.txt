add_library(mcap lib.cpp)

target_include_directories(mcap PUBLIC
  $<BUILD_INTERFACE:${MCAP_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:include/>
)

# LZ4
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
if(LZ4_FOUND)
    message(STATUS "Found LZ4 via pkg-config: ${LZ4_INCLUDE_DIRS}")
    target_include_directories(mcap PUBLIC ${LZ4_INCLUDE_DIRS})
    target_link_libraries(mcap PUBLIC ${LZ4_LIBRARIES})
else()
    message(WARNING "Dependency LZ4 not found, disabling")
    target_compile_definitions(mcap PUBLIC "MCAP_COMPRESSION_NO_LZ4")
endif()

# zstd
target_compile_definitions(mcap PUBLIC "MCAP_COMPRESSION_NO_ZSTD")