cmake_minimum_required(VERSION 3.17)

cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0042 NEW)
set (CMAKE_CXX_STANDARD 20)

# get version form package.json
execute_process(COMMAND node -p "require('./package.json').version"
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_VARIABLE PACKAGE_VERSION)
string(REGEX REPLACE "[\r\n\"]" "" PACKAGE_VERSION ${PACKAGE_VERSION})

# incldue node-addon-api dir
execute_process(COMMAND node -p "require('node-addon-api').include"
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_VARIABLE NODE_ADDON_API_DIR)
string(REGEX REPLACE "[\r\n\"]" "" NODE_ADDON_API_DIR ${NODE_ADDON_API_DIR})

# Preparation for Windows using vcpkg
if (WIN32)
	if (NOT DEFINED ENV{VCPKG_ROOT})
		message(FATAL_ERROR "$ENV{VCPKG_ROOT}: Env VCPKG_ROOT needs to be set!")
	endif ()

	set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")

	if (USE_STATIC_LIBS)
		set(VCPKG_TARGET_TRIPLET "x64-windows-static")
		set(VCPKG_HOST_TRIPLET "x64-windows-static")
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
	endif ()
endif ()

project(sftp-watch
	LANGUAGES C CXX
	VERSION "${PACKAGE_VERSION}")

string(TOLOWER "${CMAKE_SYSTEM_NAME}" PLATFORM_STR)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ARCH_STR)

if (PLATFORM_STR STREQUAL "windows")
	set(PLATFORM_STR "win32")
endif ()

if (ARCH_STR STREQUAL "x86_64" OR ARCH_STR STREQUAL "amd64")
	set(ARCH_STR "x64")
endif ()

if (ARCH_STR STREQUAL "i386" OR ARCH_STR STREQUAL "i686")
	set(ARCH_STR "ia32")
endif ()

if (ARCH_STR STREQUAL "aarch64")
	set(ARCH_STR "arm64")
endif ()

if (ARCH_STR STREQUAL "armv7l" OR ARCH_STR STREQUAL "armv6l")
	set(ARCH_STR "arm")
endif ()

set(SYSTEM_SUFFIX "${PLATFORM_STR}-${ARCH_STR}")

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INC_DIR "${SRC_DIR}/include")
set(TARGET_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
set(COMPILE_DEFS "")

if (NOT "$ENV{SFTP_READ_BUFFER_SIZE}" STREQUAL "")
	set(SFTP_READ_BUFFER_SIZE "$ENV{SFTP_READ_BUFFER_SIZE}")
endif ()

if (NOT "$ENV{SFTP_FILENAME_MAX_LEN}" STREQUAL "")
	set(SFTP_FILENAME_MAX_LEN "$ENV{SFTP_FILENAME_MAX_LEN}")
endif ()

if (DEFINED SFTP_READ_BUFFER_SIZE)
	set(COMPILE_DEFS "${COMPILE_DEFS};"
		SFTP_READ_BUFFER_SIZE=${SFTP_READ_BUFFER_SIZE})
endif ()

if (DEFINED SFTP_FILENAME_MAX_LEN)
	set(COMPILE_DEFS "${COMPILE_DEFS};"
		SFTP_FILENAME_MAX_LEN=${SFTP_FILENAME_MAX_LEN})
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(COMPILE_OPTS
		-fno-exceptions
		-fno-rtti
		-Wall
		-Wextra
		-Wfatal-errors
		-Wpedantic)

	if (CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(COMPILE_OPTS "${COMPILE_OPTS};"
			-Wold-style-cast
			-O0
			-g)
	else ()
		set(COMPILE_OPTS "${COMPILE_OPTS};"
			-Os)
	endif ()

	# prefer pthread if available
	set(THREADS_PREFER_PTHREAD_FLAG ON)

elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	set(THREADS_PREFER_PTHREAD_FLAG OFF)
	set(LINK_LIBS ws2_32)

	if (CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(COMPILE_OPTS -Zi -Od)
	else ()
		set(COMPILE_OPTS
			-Wall
			-W4
			-O2)
	endif ()

else ()
	message(FATAL_ERROR "Unknown Environment")
endif ()

if (USE_STATIC_LIBS)
	message(STATUS "USING STATIC LIBS")
	set(ZLIB_USE_STATIC_LIBS ON)
	set(OPENSSL_USE_STATIC_LIBS ON)
	set(LIBSSH2_USE_STATIC_LIBS ON)
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")

	if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
		# need to define LIBSSH2_API as empty, otherwise dll import is assumed
		set(COMPILE_DEFS "${COMPILE_DEFS};" "LIBSSH2_API=")
	endif ()
endif ()

# ------------------------ Find external Libraries -----------------------------
find_package(Threads REQUIRED)
set(LINK_LIBS Threads::Threads)

if (DEFINED ENV{LIBSSH2_LIB} AND DEFINED ENV{LIBSSH2_DIR})
	message(STATUS "Using provided libssh2 from env variable")
	set(LINK_LIBS "${LINK_LIBS};" $ENV{LIBSSH2_LIB})
	set(INC_DIR "${INC_DIR};" $ENV{LIBSSH2_DIR})
else ()
	find_package(libssh2)

	if (libssh2_FOUND)
		get_target_property(LIBSSH2_INCLUDE_DIR libssh2::libssh2
			INTERFACE_INCLUDE_DIRECTORIES)
		set(LINK_LIBS "${LINK_LIBS};" libssh2::libssh2)
		set(INC_DIR "${INC_DIR};" "${LIBSSH2_INCLUDE_DIR}")
	else ()
		find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2 REQUIRED)
		find_path(LIBSSH2_INCLUDE_DIR NAMES libssh2.h REQUIRED)

		message(STATUS "Found libssh2 ${LIBSSH2_LIBRARY}")

		set(LINK_LIBS "${LINK_LIBS};" "${LIBSSH2_LIBRARY}")
		set(INC_DIR "${INC_DIR};" "${LIBSSH2_INCLUDE_DIR}")
	endif ()
endif ()

if (DEFINED ENV{OPENSSL_SSL_LIB} AND DEFINED ENV{OPENSSL_CRYPTO_LIB} AND DEFINED ENV{OPENSSL_DIR})
	message(STATUS "Using provided OpenSSL from env variable")
	set(LINK_LIBS "${LINK_LIBS};" $ENV{OPENSSL_SSL_LIB} $ENV{OPENSSL_CRYPTO_LIB})
	set(INC_DIR "${INC_DIR};" $ENV{OPENSSL_DIR})
else ()
	find_package(OpenSSL REQUIRED)
	set(LINK_LIBS "${LINK_LIBS};" OpenSSL::SSL OpenSSL::Crypto)
endif ()

if (DEFINED ENV{ZLIB_LIB} AND DEFINED ENV{ZLIB_DIR})
	message(STATUS "Using provided ZLib from env variable")
	set(LINK_LIBS "${LINK_LIBS};" $ENV{ZLIB_LIB})
	set(INC_DIR "${INC_DIR};" $ENV{ZLIB_DIR})
else ()
	find_package(ZLIB REQUIRED)
	set(LINK_LIBS "${LINK_LIBS};" ZLIB::ZLIB)
endif ()

# ----------------------- SftpWatch C++ Libraries ------------------------------
set(SFTPWATCH_LOCAL_OBJ objSftpWatchLocal)
add_library("${SFTPWATCH_LOCAL_OBJ}" OBJECT "${SRC_DIR}/sftp_local.cc")
target_include_directories("${SFTPWATCH_LOCAL_OBJ}" PRIVATE "${INC_DIR}")
target_compile_options("${SFTPWATCH_LOCAL_OBJ}" PRIVATE "${COMPILE_OPTS}")
target_compile_definitions("${SFTPWATCH_LOCAL_OBJ}" PRIVATE ${COMPILE_DEFS})
set_target_properties("${SFTPWATCH_LOCAL_OBJ}"
	PROPERTIES POSITION_INDEPENDENT_CODE 1)

set(SFTPWATCH_REMOTE_OBJ objSftpWatchRemote)
add_library("${SFTPWATCH_REMOTE_OBJ}" OBJECT "${SRC_DIR}/sftp_remote.cc")
target_include_directories("${SFTPWATCH_REMOTE_OBJ}" PRIVATE "${INC_DIR}")
target_compile_options("${SFTPWATCH_REMOTE_OBJ}" PRIVATE "${COMPILE_OPTS}")
target_compile_definitions("${SFTPWATCH_REMOTE_OBJ}" PRIVATE ${COMPILE_DEFS})
set_target_properties("${SFTPWATCH_REMOTE_OBJ}"
	PROPERTIES POSITION_INDEPENDENT_CODE 1)

set(SFTPWATCH_ERROR_OBJ objSftpWatchError)
add_library("${SFTPWATCH_ERROR_OBJ}" OBJECT "${SRC_DIR}/sftp_err.cc")
target_include_directories("${SFTPWATCH_ERROR_OBJ}" PRIVATE "${INC_DIR}")
target_compile_options("${SFTPWATCH_ERROR_OBJ}" PRIVATE "${COMPILE_OPTS}")
target_compile_definitions("${SFTPWATCH_ERROR_OBJ}" PRIVATE ${COMPILE_DEFS})
set_target_properties("${SFTPWATCH_ERROR_OBJ}"
	PROPERTIES POSITION_INDEPENDENT_CODE 1)

set(SFTPWATCH_MAIN_OBJ objSftpWatchMain)
add_library("${SFTPWATCH_MAIN_OBJ}" OBJECT "${SRC_DIR}/sftp_watch.cc")
target_include_directories("${SFTPWATCH_MAIN_OBJ}" PRIVATE "${INC_DIR}")
target_compile_options("${SFTPWATCH_MAIN_OBJ}" PRIVATE "${COMPILE_OPTS}")
target_compile_definitions("${SFTPWATCH_MAIN_OBJ}" PRIVATE ${COMPILE_DEFS})
set_target_properties("${SFTPWATCH_MAIN_OBJ}"
	PROPERTIES POSITION_INDEPENDENT_CODE 1)

# -------------------- SftpWatch Node-API Libraries ----------------------------
add_library("${PROJECT_NAME}"
	SHARED
		${CMAKE_JS_SRC}
		"${SRC_DIR}/sftp_node_api.cc"
		"$<TARGET_OBJECTS:${SFTPWATCH_LOCAL_OBJ}>"
		"$<TARGET_OBJECTS:${SFTPWATCH_REMOTE_OBJ}>"
		"$<TARGET_OBJECTS:${SFTPWATCH_ERROR_OBJ}>"
		"$<TARGET_OBJECTS:${SFTPWATCH_MAIN_OBJ}>")

set_target_properties("${PROJECT_NAME}"
	PROPERTIES
		POSITION_INDEPENDENT_CODE 1
		LIBRARY_OUTPUT_DIRECTORY "${TARGET_LIB_DIR}"
		LINK_FLAGS_RELEASE -s
		PREFIX ""
		SUFFIX ".${SYSTEM_SUFFIX}.node")

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	# Windows .dll is treated as runtime not library
	set_target_properties("${PROJECT_NAME}"
		PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY "${TARGET_LIB_DIR}"
			RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TARGET_LIB_DIR}"
			RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TARGET_LIB_DIR}")
endif ()

target_include_directories("${PROJECT_NAME}"
	PRIVATE
		"${CMAKE_JS_INC}"
		"${NODE_ADDON_API_DIR}"
		"${NAPI_INCLUDE_DIR}"
		"${INC_DIR}")

target_compile_options("${PROJECT_NAME}" PRIVATE ${COMPILE_OPTS})

target_link_libraries("${PROJECT_NAME}"
	PRIVATE
		${CMAKE_JS_LIB}
		${LINK_LIBS})

target_compile_definitions("${PROJECT_NAME}"
	PRIVATE
		${COMPILE_DEFS}
		NAPI_DISABLE_CPP_EXCEPTIONS
		NAPI_VERSION=6)
