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

typedef struct SftpWatch_s SftpWatch_t;

struct SftpWatch_s {
	uint32_t    id;
	std::string host;
	uint16_t    port;
	std::string pubkey;
	std::string privkey;
	std::string username;
	std::string password;
	std::string path;

	libssh2_socket_t     sock;
	LIBSSH2_SESSION*     session = NULL;
	LIBSSH2_SFTP*        sftp_session;
	LIBSSH2_SFTP_HANDLE* sftp_handle;
	struct sockaddr_in   sin;
	const char*          fingerprint;

	Napi::Promise::Deferred  deferred;
	std::thread              thread;
	Napi::ThreadSafeFunction tsfn;

	SftpWatch_s(Napi::Env env, uint32_t qid)
		: id(qid)
		, deferred(Napi::Promise::Deferred::New(env))
	{
		// intended empty function
	}
};

#endif
