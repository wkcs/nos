# Each SoC contributes its own header paths; unrelated paths must not leak in.
set(CONFIG_SOC_STM32F1 TRUE)
set(CONFIG_SOC_STM32F4 FALSE)
set(CONFIG_SOC_NUC472 FALSE)
set(_NOS_INCLUDE_DIRS "")
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/../soc/headers.cmake")
if(NOT "${ROOT}/soc/stm32f1/include" IN_LIST _NOS_INCLUDE_DIRS)
    message(FATAL_ERROR "STM32F1 public headers missing: ${_NOS_INCLUDE_DIRS}")
endif()
if("${ROOT}/soc/stm32f4/include" IN_LIST _NOS_INCLUDE_DIRS OR
   "${ROOT}/soc/nuc472/include" IN_LIST _NOS_INCLUDE_DIRS)
    message(FATAL_ERROR "Other SoC headers leaked into STM32F1 configuration")
endif()

set(CONFIG_SOC_STM32F1 FALSE)
set(CONFIG_SOC_STM32F4 TRUE)
set(CONFIG_SOC_NUC472 FALSE)
set(_NOS_INCLUDE_DIRS "")
include("${CMAKE_CURRENT_LIST_DIR}/../soc/headers.cmake")
if(NOT "${ROOT}/soc/stm32f4/include" IN_LIST _NOS_INCLUDE_DIRS OR
   "${ROOT}/soc/stm32f1/include" IN_LIST _NOS_INCLUDE_DIRS OR
   "${ROOT}/soc/nuc472/include" IN_LIST _NOS_INCLUDE_DIRS)
    message(FATAL_ERROR "Incorrect STM32F4 header selection: ${_NOS_INCLUDE_DIRS}")
endif()

set(CONFIG_SOC_STM32F1 FALSE)
set(CONFIG_SOC_STM32F4 FALSE)
set(CONFIG_SOC_NUC472 TRUE)
set(_NOS_INCLUDE_DIRS "")
include("${CMAKE_CURRENT_LIST_DIR}/../soc/headers.cmake")
if(NOT "${ROOT}/soc/nuc472/include" IN_LIST _NOS_INCLUDE_DIRS OR
   "${ROOT}/soc/stm32f1/include" IN_LIST _NOS_INCLUDE_DIRS OR
   "${ROOT}/soc/stm32f4/include" IN_LIST _NOS_INCLUDE_DIRS)
    message(FATAL_ERROR "Incorrect NUC472 header selection: ${_NOS_INCLUDE_DIRS}")
endif()

foreach(_dir IN LISTS _NOS_INCLUDE_DIRS)
    if("${_dir}" MATCHES ".*/board/.*")
        message(FATAL_ERROR "board/ path leaked into _NOS_INCLUDE_DIRS: ${_dir}")
    endif()
endforeach()
