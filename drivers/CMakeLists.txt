set(DRIVERS_SRCS "")

foreach(_driver base block display i2c key misc serial spi flash usb)
    include("${CMAKE_CURRENT_SOURCE_DIR}/${_driver}/sources.cmake")
endforeach()

add_library(nos_drivers OBJECT ${DRIVERS_SRCS})
nos_configure_kernel_target(nos_drivers)
