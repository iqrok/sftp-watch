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

project(sftp-watch
	LANGUAGES C CXX
	VERSION 0.1.0)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INC_DIR "${SRC_DIR}/include")

if (NOT "$ENV{SFTP_READ_BUFFER_SIZE}" STREQUAL "")
	set(SFTP_READ_BUFFER_SIZE "$ENV{SFTP_READ_BUFFER_SIZE}")
endif ()

if (NOT "$ENV{SFTP_FILENAME_MAX_LEN}" STREQUAL "")
	set(SFTP_FILENAME_MAX_LEN "$ENV{SFTP_FILENAME_MAX_LEN}")
endif ()

add_library("${PROJECT_NAME}"
	SHARED
		"${SRC_DIR}/sftp_helper.cc"
		"${SRC_DIR}/sftp_node.cc")

set_target_properties("${PROJECT_NAME}"
	PROPERTIES POSITION_INDEPENDENT_CODE 1
	PREFIX ""
	SUFFIX ".node")

target_include_directories("${PROJECT_NAME}"
	PRIVATE
		"${CMAKE_JS_INC}"
		"${NODE_ADDON_API_DIR}"
		"${NAPI_INCLUDE_DIR}"
		"${INC_DIR}")

target_compile_definitions("${PROJECT_NAME}"
	PRIVATE
		NAPI_DISABLE_CPP_EXCEPTIONS
		NAPI_VERSION=6)

if (DEFINED SFTP_READ_BUFFER_SIZE)
	target_compile_definitions("${PROJECT_NAME}"
		PRIVATE SFTP_READ_BUFFER_SIZE=${SFTP_READ_BUFFER_SIZE})
endif ()

if (DEFINED SFTP_FILENAME_MAX_LEN)
	target_compile_definitions("${PROJECT_NAME}"
		PRIVATE SFTP_FILENAME_MAX_LEN=${SFTP_FILENAME_MAX_LEN})
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_compile_options("${PROJECT_NAME}"
		PRIVATE
			#~ -EHsc # enable exception
			-W4   # equivalent of -Wall
			-O2)

	target_link_libraries("${PROJECT_NAME}" PRIVATE ws2_32)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options("${PROJECT_NAME}"
		PRIVATE
			#~ -fexceptions # enable exception
			-Wall
			-Wextra
			-Wfatal-errors
			-Wpedantic
			-O2
		)
else ()
	message(FATAL_ERROR "Unknown Environment")
endif ()

# pthread
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries("${PROJECT_NAME}" PRIVATE Threads::Threads)

if (DEFINED ENV{ZLIB_LIB} AND DEFINED ENV{ZLIB_DIR})
	target_link_libraries("${PROJECT_NAME}" PRIVATE "$ENV{ZLIB_LIB}")
	target_include_directories("${PROJECT_NAME}" PRIVATE "$ENV{ZLIB_DIR}")
else ()
	find_package(ZLIB REQUIRED)
	target_link_libraries("${PROJECT_NAME}" PRIVATE ZLIB::ZLIB)
endif ()

if (DEFINED ENV{OPENSSL_SSL_LIB} AND DEFINED ENV{OPENSSL_CRYPTO_LIB} AND DEFINED ENV{OPENSSL_DIR})
	target_link_libraries("${PROJECT_NAME}"
		PRIVATE
			"$ENV{OPENSSL_SSL_LIB}"
			"$ENV{OPENSSL_CRYPTO_LIB}")
	target_include_directories("${PROJECT_NAME}" PRIVATE "$ENV{OPENSSL_DIR}")
else ()
	find_package(OpenSSL REQUIRED)
	target_link_libraries("${PROJECT_NAME}"
		PRIVATE
			OpenSSL::SSL
			OpenSSL::Crypto)
endif ()

if (DEFINED ENV{LIBSSH2_LIB} AND DEFINED ENV{LIBSSH2_DIR})
	target_link_libraries("${PROJECT_NAME}" PRIVATE $ENV{LIBSSH2_LIB})
	target_include_directories("${PROJECT_NAME}" PRIVATE "$ENV{LIBSSH2_DIR}")
else ()
	find_package(libssh2 QUIET)

	if (${libssh2_FOUND})
		target_link_libraries("${PROJECT_NAME}"
			PRIVATE libssh2::libssh2)
	else ()
		find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2)
		find_path(LIBSSH2_INCLUDE_DIR NAMES libssh2.h)

		target_link_libraries("${PROJECT_NAME}"
			PRIVATE "${LIBSSH2_LIBRARY}")

		target_include_directories("${PROJECT_NAME}"
			PRIVATE "${LIBSSH2_INCLUDE_DIR}")
	endif ()
endif ()
