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

#~ add_definitions(-DNAPI_VERSION=6 -DNAPI_CPP_EXCEPTIONS)
add_definitions(-DNAPI_VERSION=6 -DNAPI_DISABLE_CPP_EXCEPTIONS)

add_library("${PROJECT_NAME}" SHARED
		"${SRC_DIR}/sftp_nonblock.cc"
	)
set_target_properties("${PROJECT_NAME}"
	PROPERTIES POSITION_INDEPENDENT_CODE 1
	PREFIX ""
	SUFFIX ".node")

target_include_directories("${PROJECT_NAME}" PRIVATE
	"${CMAKE_JS_INC}"
	"${NODE_ADDON_API_DIR}"
	"${NAPI_INCLUDE_DIR}"
	"${INC_DIR}")

target_compile_options("${PROJECT_NAME}" PRIVATE
	-Wall
	-Wfatal-errors
	-O2
	-fexceptions
	-Wpedantic)

# pthread
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries("${PROJECT_NAME}"
	PRIVATE Threads::Threads
		OpenSSL::SSL
		OpenSSL::Crypto
		ZLIB::ZLIB)

find_package(libssh2)

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
