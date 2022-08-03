if (PLATFORM STREQUAL "posix")
	## rb5 flight
	include(px4_git)
	list(APPEND CMAKE_MODULE_PATH
	"${PX4_SOURCE_DIR}/platforms/posix/cmake"
	)
	set(QC_SOC_TARGET "QRB5165")

	set(DISABLE_PARAMS_MODULE_SCOPING TRUE)

	set(CONFIG_SHMEM "0")
	add_definitions(-DORB_COMMUNICATOR)
	add_definitions(-DRELEASE_BUILD)

	set(CONFIG_PARAM_SERVER "1")

	add_compile_options($<$<COMPILE_LANGUAGE:C>:-std=gnu99>)
	add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++14>)

	add_compile_options(
	-Wno-array-bounds
	)

	add_definitions(
	-D__PX4_POSIX_RB5
	-D__PX4_LINUX
	)

	link_directories(/home ${PX4_SOURCE_DIR}/boards/modalai/rb5-flight/lib)
elseif (PLATFORM STREQUAL "qurt")
	message(STATUS "*** Entering qurt.cmake ***")
	set(QC_SOC_TARGET "QRB5165")
	include(px4_git)
	list(APPEND CMAKE_MODULE_PATH
		"${PX4_SOURCE_DIR}/platforms/qurt/cmake"
	)


	if ("$ENV{HEXAGON_SDK_ROOT}" STREQUAL "")
	message(FATAL_ERROR "Enviroment variable HEXAGON_SDK_ROOT must be set")
	else()
	set(HEXAGON_SDK_ROOT $ENV{HEXAGON_SDK_ROOT})
	endif()

	include(Toolchain-qurt)
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

	set(DISABLE_PARAMS_MODULE_SCOPING TRUE)

	add_definitions(-D__PX4_QURT_EXCELSIOR)
endif()
