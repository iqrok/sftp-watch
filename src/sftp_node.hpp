#ifndef _SFTP_NODE_HPP
#define _SFTP_NODE_HPP

#include <cstdint>
#include <map>
#include <thread>

#include <napi.h>

#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef __linux__
#	include <sys/socket.h>
#	include <unistd.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#endif

constexpr uint16_t filename_max_len = 512;

enum FileType_e {
	IS_INVALID = 0,
	IS_SYMLINK,
	IS_REG_FILE,
	IS_DIR,
	IS_CHR_FILE,
	IS_BLK_FILE,
	IS_PIPE,
	IS_SOCK,
};

typedef struct DirItem_s {
	uint8_t type = 0;
	char    name[filename_max_len];

	LIBSSH2_SFTP_ATTRIBUTES attrs;
	/*
	 * LIBSSH2_SFTP_S_ISLNK  Test for a symbolic link
	 * LIBSSH2_SFTP_S_ISREG  Test for a regular file
	 * LIBSSH2_SFTP_S_ISDIR  Test for a directory
	 * LIBSSH2_SFTP_S_ISCHR  Test for a character special file
	 * LIBSSH2_SFTP_S_ISBLK  Test for a block special file
	 * LIBSSH2_SFTP_S_ISFIFO Test for a pipe or FIFO special file
	 * LIBSSH2_SFTP_S_ISSOCK Test for a socket
	 * */
} DirItem_t;

typedef struct SftpWatch_s {
	uint32_t    id;
	std::string host;
	uint16_t    port;
	std::string pubkey;
	std::string privkey;
	std::string username;
	std::string password;

	libssh2_socket_t     sock;
	LIBSSH2_SESSION*     session = NULL;
	LIBSSH2_SFTP*        sftp_session;
	LIBSSH2_SFTP_HANDLE* sftp_handle;
	struct sockaddr_in   sin;
	const char*          fingerprint;

	Napi::Promise::Deferred  deferred;
	std::thread              thread;
	Napi::ThreadSafeFunction tsfn;

	std::map<std::string, DirItem_t> files;

	bool     is_stopped;
	uint64_t delay_us = 1'000'000;

	SftpWatch_s(Napi::Env env, uint32_t qid)
		: id(qid)
		, deferred(Napi::Promise::Deferred::New(env))
	{
		// left empty intentionally
	}
} SftpWatch_t;

#endif
