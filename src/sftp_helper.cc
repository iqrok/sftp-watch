#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef __linux__
#	include <netdb.h>
#	include <sys/socket.h>
#	include <unistd.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#	include <sys/time.h>
#	include <utime.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "sftp_helper.hpp"

static bool is_inited = false;

#define FN_ACTUAL_ERROR(err)   ((err) != LIBSSH2_ERROR_EAGAIN)
#define FN_LAST_ERRNO_ERROR(s) FN_ACTUAL_ERROR(libssh2_session_last_errno(s))

// Straight copied from example/sftp_RW_nonblock.c
static int waitsocket(libssh2_socket_t socket_fd, LIBSSH2_SESSION* session)
{
	struct timeval timeout;
	int            rc;
	fd_set         fd;
	fd_set*        writefd = NULL;
	fd_set*        readfd  = NULL;
	int            dir;

	timeout.tv_sec  = 10;
	timeout.tv_usec = 0;

	FD_ZERO(&fd);

#if defined(__GNUC__) || defined(__clang__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
	FD_SET(socket_fd, &fd);
#if defined(__GNUC__) || defined(__clang__)
#	pragma GCC diagnostic pop
#endif

	/* now make sure we wait in the correct direction */
	dir = libssh2_session_block_directions(session);

	if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) readfd = &fd;

	if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) writefd = &fd;

	rc = select((int)(socket_fd + 1), readfd, writefd, NULL, &timeout);

	return rc;
}

int32_t SftpHelper::connect(SftpWatch_t* ctx)
{
	int32_t rc;

	if (!is_inited) {
		if ((rc = libssh2_init(0)) != 0) {
			fprintf(stderr, "libssh2 initialization failed (%d)\n", rc);
			return -127;
		}

		is_inited = true;
	}

	struct addrinfo  hints;
	struct addrinfo* res = NULL;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	rc = getaddrinfo(
		ctx->host.c_str(), std::to_string(ctx->port).c_str(), &hints, &res);
	if (rc != 0 || !res) {
		printf("FAILED getaddrinfo %d %d\n", rc, errno);
		if (res) freeaddrinfo(res);

		return -1;
	}

	ctx->sock = socket(res->ai_family, res->ai_socktype, 0);
	if (ctx->sock == LIBSSH2_INVALID_SOCKET) {
		fprintf(stderr, "failed to create socket.\n");
		freeaddrinfo(res);

		return -1;
	}

	rc = connect(ctx->sock, res->ai_addr, res->ai_addrlen);
	if (rc) {
		fprintf(stderr, "failed to connect. (%d) [%s:%u]\n", rc,
			ctx->host.c_str(), ctx->port);
		freeaddrinfo(res);

		return rc;
	}

	/* Create a session instance */
	ctx->session = libssh2_session_init();
	if (!ctx->session) {
		fprintf(stderr, "Could not initialize SSH session.\n");
		return -1;
	}

	/* Since we have set non-blocking, tell libssh2 we are non-blocking */
	libssh2_session_set_blocking(ctx->session, 0);

	while ((rc = libssh2_session_handshake(ctx->session, ctx->sock))
		== LIBSSH2_ERROR_EAGAIN);

	if (rc) {
		fprintf(stderr, "Failure establishing SSH session: %d\n", rc);
		return -1;
	}

	ctx->fingerprint
		= libssh2_hostkey_hash(ctx->session, LIBSSH2_HOSTKEY_HASH_SHA1);

	fprintf(stderr, "Fingerprint: ");
	for (uint8_t i = 0; i < 20; i++) {
		fprintf(stderr, "%02X ", (unsigned char)ctx->fingerprint[i]);
	}
	fprintf(stderr, "\n");

	return 0;
}

int32_t SftpHelper::auth(SftpWatch_t* ctx)
{
	int rc;

	/*
	 * Authentication will prioritize pubkey over password.
	 * A valid pubkey auth needs both pubkey and privkey to be not empty.
	 *
	 * If one of pubkey or privkey is empty, then password authentication will
	 * be performed only if password is not empty.
	 *
	 * Otherwise error will be returned. In short words, a valid auth method
	 * can be determined based on this condition
	 *
	 * `valid = (!pubkey.empty() && !privkey.empty()) || !password.empty()`
	 * */
	if (!ctx->pubkey.empty() && !ctx->privkey.empty()) {
		while ((rc = libssh2_userauth_publickey_fromfile(ctx->session,
					ctx->username.c_str(), ctx->pubkey.c_str(),
					ctx->privkey.c_str(), ctx->password.c_str()))
			== LIBSSH2_ERROR_EAGAIN);

		if (rc) {
			fprintf(stderr, "Authentication by public key failed.\n");
			return -1;
		}
	} else if (!ctx->password.empty()) {
		while ((rc = libssh2_userauth_password(
					ctx->session, ctx->username.c_str(), ctx->password.c_str()))
			== LIBSSH2_ERROR_EAGAIN);

		if (rc) {
			fprintf(stderr, "Authentication by password failed.\n");
			return -1;
		}
	} else {
		fprintf(stderr, "No Valid Authentication is provided.\n");
		return -2;
	}

	do {
		ctx->sftp_session = libssh2_sftp_init(ctx->session);

		if (!ctx->sftp_session && FN_LAST_ERRNO_ERROR(ctx->session)) {
			fprintf(stderr, "Unable to init SFTP session\n");
			return -3;
		}
	} while (!ctx->sftp_session);

	return 0;
}

int32_t SftpHelper::open_dir(SftpWatch_t* ctx, const char* remote_path)
{
	do {
		ctx->sftp_handle = libssh2_sftp_opendir(ctx->sftp_session, remote_path);

		if (!ctx->sftp_handle && FN_LAST_ERRNO_ERROR(ctx->session)) {
			fprintf(stderr, "Unable to open dir '%s' with SFTP\n",
				remote_path);
			return -1;
		}
	} while (!ctx->sftp_handle);

	return 0;
}

int32_t SftpHelper::read_dir(SftpWatch_t* ctx, DirItem_t* file)
{
	int rc = 0;

	char filename[SFTP_FILENAME_MAX_LEN];
	while ((rc = libssh2_sftp_readdir(
				ctx->sftp_handle, filename, sizeof(filename), &file->attrs))
		== LIBSSH2_ERROR_EAGAIN);

	// there's a record
	if (rc > 0) {
		file->name = std::string(filename);
		file->type = SftpHelper::get_filetype(file);
		return 1;
	}

	// should try again
	if (rc == LIBSSH2_ERROR_EAGAIN) return 2;

	// read dir is finished
	return 0;
}

uint8_t SftpHelper::get_filetype(DirItem_t* file)
{
	if (file->attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
		if (LIBSSH2_SFTP_S_ISREG(file->attrs.permissions)) {
			return IS_REG_FILE;
		} else if (LIBSSH2_SFTP_S_ISDIR(file->attrs.permissions)) {
			return IS_DIR;
		} else if (LIBSSH2_SFTP_S_ISLNK(file->attrs.permissions)) {
			return IS_SYMLINK;
		} else if (LIBSSH2_SFTP_S_ISCHR(file->attrs.permissions)) {
			return IS_CHR_FILE;
		} else if (LIBSSH2_SFTP_S_ISBLK(file->attrs.permissions)) {
			return IS_BLK_FILE;
		} else if (LIBSSH2_SFTP_S_ISFIFO(file->attrs.permissions)) {
			return IS_PIPE;
		} else if (LIBSSH2_SFTP_S_ISSOCK(file->attrs.permissions)) {
			return IS_SOCK;
		} else {
			return IS_INVALID;
		}
	} else {
		return IS_INVALID;
	}
}

void SftpHelper::cleanup(SftpWatch_t* ctx)
{
	libssh2_sftp_close(ctx->sftp_handle);
	libssh2_sftp_shutdown(ctx->sftp_session);

	if (ctx->session) {
		libssh2_session_disconnect(ctx->session, "Normal Shutdown");
		libssh2_session_free(ctx->session);
	}

	if (ctx->sock != LIBSSH2_INVALID_SOCKET) {
		shutdown(ctx->sock, 2);
		LIBSSH2_SOCKET_CLOSE(ctx->sock);
	}

	libssh2_exit();

#ifdef _WIN32
	WSACleanup();
#endif
}

int32_t SftpHelper::sync_remote(SftpWatch_t* ctx, DirItem_t* file)
{
	std::string remote_file = ctx->remote_path + std::string("/") + file->name;
	std::string local_file  = ctx->local_path + std::string("/") + file->name;

	LIBSSH2_SFTP_HANDLE* handle = NULL;

	// try to open remote file and wait until socket ready
	do {
		handle = libssh2_sftp_open(
			ctx->sftp_session, remote_file.c_str(), LIBSSH2_FXF_READ, 0);

		if (!handle) {
			if (FN_LAST_ERRNO_ERROR(ctx->session)) {
				fprintf(stderr, "Unable to open file with SFTP: %ld\n",
					libssh2_sftp_last_error(ctx->sftp_session));
				return -1;
			} else {
				// non-blocking open, now we wait until socket is ready
				waitsocket(ctx->sock, ctx->session);
			}
		}
	} while (!handle);

	if (!handle) {
		fprintf(stderr, "Unable to open file with SFTP: %ld\n",
			libssh2_sftp_last_error(ctx->sftp_session));
		return -1;
	}

	int32_t err      = 0;
	FILE*   fd_local = fopen(local_file.c_str(), "wb");

	if (!fd_local) {
		fprintf(stderr, "Error opening file!\n");
		return -2;
	}

	do {
		int32_t nread = 0;

		do {
			char mem[SFTP_READ_BUFFER_SIZE];

			/* read in a loop until we block */
			nread = libssh2_sftp_read(handle, mem, sizeof(mem));
			if (nread > 0) fwrite(mem, (size_t)nread, 1, fd_local);
		} while (nread > 0);

		// error or end of file
		if (nread != LIBSSH2_ERROR_EAGAIN) {
			// set modification time for local file to match the remote one
			struct utimbuf times = {
				.actime  = (time_t)file->attrs.atime,
				.modtime = (time_t)file->attrs.mtime,
			};

			utime(local_file.c_str(), &times);

			break;
		}

		struct timeval timeout;
		fd_set         fd;
		fd_set         fd2;

		timeout.tv_sec  = 10;
		timeout.tv_usec = 0;

		FD_ZERO(&fd);
		FD_ZERO(&fd2);
#if defined(__GNUC__) || defined(__clang__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
		FD_SET(ctx->sock, &fd);
		FD_SET(ctx->sock, &fd2);
#if defined(__GNUC__) || defined(__clang__)
#	pragma GCC diagnostic pop
#endif

		/* wait for readable or writeable */
		int32_t rc = select((int)(ctx->sock + 1), &fd, &fd2, NULL, &timeout);
		if (rc <= 0) {
			/* negative is error, 0 is timeout */
			fprintf(stderr, "SFTP download timed out: %d\n", rc);

			err = rc == 0 ? -3 : -1;
			break;
		}
	} while (1);

	libssh2_sftp_close(handle);
	fclose(fd_local);

	return err;
}
