set(APP_NAME GripperHmiTool)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

set(GRIPPER_HMI_HAS_SYSTEM_LIBSERIALPORT OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSERIALPORT QUIET IMPORTED_TARGET libserialport)
    if(LIBSERIALPORT_FOUND)
        set(GRIPPER_HMI_HAS_SYSTEM_LIBSERIALPORT ON)
    endif()
endif()

set(GRIPPER_HMI_LIBSERIALPORT_TARGET "")
set(GRIPPER_HMI_LIBSERIALPORT_LIBRARY_DIR "")
if(GRIPPER_HMI_HAS_SYSTEM_LIBSERIALPORT)
    set(GRIPPER_HMI_LIBSERIALPORT_TARGET PkgConfig::LIBSERIALPORT)
    message(STATUS "GripperHmiTool: using system libserialport")
else()
    set(GRIPPER_HMI_UGRIPPER_ROOT "${CMAKE_SOURCE_DIR}")
    set(GRIPPER_HMI_LIBSERIALPORT_ROOT
        "${GRIPPER_HMI_UGRIPPER_ROOT}/.local-deps/libserialport/root/usr")
    set(GRIPPER_HMI_LIBSERIALPORT_INCLUDE_DIR
        "${GRIPPER_HMI_LIBSERIALPORT_ROOT}/include")
    set(GRIPPER_HMI_LIBSERIALPORT_LIBRARY
        "${GRIPPER_HMI_LIBSERIALPORT_ROOT}/lib/x86_64-linux-gnu/libserialport.so")
    set(GRIPPER_HMI_LIBSERIALPORT_LIBRARY_DIR
        "${GRIPPER_HMI_LIBSERIALPORT_ROOT}/lib/x86_64-linux-gnu")

    if(EXISTS "${GRIPPER_HMI_LIBSERIALPORT_INCLUDE_DIR}/libserialport.h" AND
       EXISTS "${GRIPPER_HMI_LIBSERIALPORT_LIBRARY}")
        add_library(gripper_hmi_local_libserialport SHARED IMPORTED GLOBAL)
        set_target_properties(gripper_hmi_local_libserialport PROPERTIES
            IMPORTED_LOCATION "${GRIPPER_HMI_LIBSERIALPORT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GRIPPER_HMI_LIBSERIALPORT_INCLUDE_DIR}"
        )
        set(GRIPPER_HMI_LIBSERIALPORT_TARGET gripper_hmi_local_libserialport)
        message(WARNING
            "GripperHmiTool: system libserialport not found; using sibling ugripper .local-deps fallback for x86 validation")
    else()
        message(FATAL_ERROR
            "GripperHmiTool requires libserialport. Install libserialport-dev or provide sibling ugripper .local-deps/libserialport.")
    endif()
endif()

add_library(gripper_hmi_protocol STATIC
    gripper_hmi_protocol.cc
)
set_target_properties(gripper_hmi_protocol PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(gripper_hmi_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(gripper_hmi_led_effects STATIC
    gripper_hmi_led_effects.cc
)
set_target_properties(gripper_hmi_led_effects PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(gripper_hmi_led_effects
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gripper_hmi_led_effects
    PUBLIC
        gripper_hmi_protocol
)

add_library(gripper_hmi STATIC
    gripper_hmi_driver.cc
)
set_target_properties(gripper_hmi PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(gripper_hmi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gripper_hmi
    PUBLIC
        gripper_hmi_led_effects
        ${GRIPPER_HMI_LIBSERIALPORT_TARGET}
    PRIVATE
        PP::utils
)

add_executable(${APP_NAME}
    main.cc
)
set_target_properties(${APP_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "GripperHmiTool"
)
target_include_directories(${APP_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(${APP_NAME}
    PRIVATE
        gripper_hmi
        PP::utils
        Threads::Threads
)

if(NOT GRIPPER_HMI_HAS_SYSTEM_LIBSERIALPORT)
    set_target_properties(${APP_NAME} PROPERTIES
        BUILD_RPATH "${GRIPPER_HMI_LIBSERIALPORT_LIBRARY_DIR}"
        INSTALL_RPATH "${GRIPPER_HMI_LIBSERIALPORT_LIBRARY_DIR}"
    )
endif()

install(TARGETS ${APP_NAME}
    RUNTIME DESTINATION bin/GripperHmiTool
    LIBRARY DESTINATION lib
)
