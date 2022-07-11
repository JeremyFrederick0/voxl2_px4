
# Excelsior is the code name of a board currently in development.
#
# This cmake config builds for QURT which is the operating system running on
# the DSP side.

message(STATUS "*** Entering qurt.cmake ***")
set(QC_SOC_TARGET "QRB5165")

include(px4_git)
# px4_add_git_submodule(TARGET git_cmake_hexagon PATH "${PX4_SOURCE_DIR}/boards/modalai/cmake_hexagon")
list(APPEND CMAKE_MODULE_PATH "${PX4_SOURCE_DIR}/boards/modalai/cmake_hexagon")

if ("$ENV{HEXAGON_SDK_ROOT}" STREQUAL "")
	message(FATAL_ERROR "Enviroment variable HEXAGON_SDK_ROOT must be set")
else()
	set(HEXAGON_SDK_ROOT $ENV{HEXAGON_SDK_ROOT})
endif()

include(toolchain/Toolchain-qurt)
message(STATUS "in qurt.make before qurt_flags.cmake")
include(qurt_flags)
message(STATUS "in qurt.make after qurt_flags.cmake")
message(STATUS "in qurt.make: CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")

set(HEXAGON_SDK_INCLUDES ${HEXAGON_SDK_INCLUDES}
    ${HEXAGON_SDK_ROOT}/tools/HEXAGON_Tools/8.4.05/Tools/target/hexagon/include
    ${PX4_SOURCE_DIR}/platforms/nuttx/Nuttx/nuttx/include
       )
include_directories(${HEXAGON_SDK_INCLUDES})

set(CONFIG_SHMEM "0")
add_definitions(-DORB_COMMUNICATOR)
# add_definitions(-DDEBUG_BUILD)
add_definitions(-DRELEASE_BUILD)

set(CONFIG_PARAM_CLIENT "1")

# Disable the creation of the parameters.xml file by scanning individual
# source files, and scan all source files.  This will create a parameters.xml
# file that contains all possible parameters, even if the associated module
# is not used.  This is necessary for parameter synchronization between the
# ARM and DSP processors.
set(DISABLE_PARAMS_MODULE_SCOPING TRUE)

# This definition allows to differentiate the specific board.
add_definitions(-D__PX4_QURT_EXCELSIOR)

px4_add_board(
	PLATFORM qurt
	VENDOR modalai
	MODEL excelsior
	LABEL qurt
	DRIVERS
	MODULES
		muorb/slpi
	SYSTEMCMDS
	)

message(STATUS "*** Exiting qurt.cmake ***")
