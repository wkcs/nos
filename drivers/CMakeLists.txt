set(DRIVERS_SRCS "")

foreach(_driver base block display gpio i2c misc serial spi flash usb)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_driver}/sources.cmake")
        include("${CMAKE_CURRENT_SOURCE_DIR}/${_driver}/sources.cmake")
    endif()
endforeach()

add_library(nos_drivers OBJECT ${DRIVERS_SRCS})
nos_configure_kernel_target(nos_drivers)
