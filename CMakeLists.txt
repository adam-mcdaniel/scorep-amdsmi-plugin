cmake_minimum_required(VERSION 3.28)
project(amd_smi_plugin)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/common;${CMAKE_MODULE_PATH}")

OPTION(BACKEND_SCOREP "Build plugin using scorep(ON) or vampirtrace(OFF)" ON)

set(PLUGIN_SOURCE apapi.c)

add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCE})
target_link_libraries(${PROJECT_NAME} PRIVATE pthread m)

#additional c flags
set(CMAKE_C_FLAGS "-D_GNU_SOURCE -std=c11 -O2 -g")
#debugging c flags
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DHAVE_DEBUG -O0 -Wstrict-prototypes -Wall -Wundef -Wno-long-long -Wsign-compare -Wcomment -pedantic -finline-functions -fno-strict-aliasing")
#release c flags
set(CMAKE_C_FLAGS_RELEASE "-Os")


if(BACKEND_SCOREP)
    include(common/FindScorep.cmake)
    if(SCOREP_FOUND)
	target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${SCOREP_INCLUDE_DIRS})
        target_compile_definitions(${PROJECT_NAME} PRIVATE "-DBACKEND_SCOREP")
    else()
        message("Score-P was not found, falling back to VampirTrace!")
    endif()
endif()

if(NOT SCOREP_FOUND OR NOT BACKEND_SCOREP)
    include(common/FindVampirTrace.cmake)
    if(VT_FOUND)
        target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${VT_INCLUDE_DIRS})
        target_compile_definitions(${PROJECT_NAME} PRIVATE "-DBACKEND_VTRACE")
    else()
        message(SEND_ERROR "Found neither Score-P nor VampirTrace backend!")
    endif()
endif()

find_package(PAPI REQUIRED)
option(LINK_WITH_PAPI "Link this plugin directly with libpapi.so" ON)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${PAPI_INCLUDE_DIRS})
if(LINK_WITH_PAPI)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${PAPI_LIBRARIES})
endif()

# Asegurar que el enlazador busque en el directorio de bibliotecas de ROCm
target_link_directories(amd_smi_plugin PRIVATE /opt/rocm-7.2.0/lib)

# Enlazar explícitamente la biblioteca libamd_smi.so
target_link_libraries(amd_smi_plugin PRIVATE amd_smi)

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION lib)
