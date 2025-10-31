#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef __linux__
#	include <netdb.h>
#	include <sys/socket.h>
#	include <unistd.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#endif

#include <string>

#include "sftp_helper.hpp"

static bool is_inited = true;

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

	rc = getaddrinfo(ctx->host.c_str(), std::to_string(ctx->port).c_str(), &hints, &res);
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

	// pubkey auth if both pubkey and privkey are provided
	if (!ctx->pubkey.empty() && !ctx->privkey.empty()) {
		while ((rc = libssh2_userauth_publickey_fromfile(ctx->session,
					ctx->username.c_str(), ctx->pubkey.c_str(),
					ctx->privkey.c_str(), ctx->password.c_str()))
			== LIBSSH2_ERROR_EAGAIN);

		if (rc) {
			fprintf(stderr, "Authentication by public key failed.\n");
			return -1;
		}
	}

	// or password if privkey is not provided
	else if (!ctx->password.empty()) {
		while ((rc = libssh2_userauth_password(
					ctx->session, ctx->username.c_str(), ctx->password.c_str()))
			== LIBSSH2_ERROR_EAGAIN);

		if (rc) {
			fprintf(stderr, "Authentication by password failed.\n");
			return -1;
		}
	}

	// welp, no password no pubkey, wtf do you want!? is the admin stupid.
	else {
		fprintf(stderr, "No Valid Authentication is provided.\n");
		return -2;
	}

	//~ fprintf(stderr, "libssh2_sftp_init().\n");
	do {
		ctx->sftp_session = libssh2_sftp_init(ctx->session);

		if (!ctx->sftp_session
			&& libssh2_session_last_errno(ctx->session)
				!= LIBSSH2_ERROR_EAGAIN) {
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

		if (!ctx->sftp_handle
			&& libssh2_session_last_errno(ctx->session)
				!= LIBSSH2_ERROR_EAGAIN) {
			fprintf(stderr, "Unable to open dir '%s' with SFTP\n", remote_path);
			return -1;
		}
	} while (!ctx->sftp_handle);

	return 0;
}

int32_t SftpHelper::read_dir(SftpWatch_t* ctx, DirItem_t* file)
{
	int rc = 0;

	while ((rc = libssh2_sftp_readdir(ctx->sftp_handle, file->name,
				filename_max_len, &file->attrs))
		== LIBSSH2_ERROR_EAGAIN);

	// either there's a record or should try again
	if (rc > 0) {
		file->type = SftpHelper::get_filetype(file);
		return 1;
	}

	if (rc == LIBSSH2_ERROR_EAGAIN) return 2;

	// read dir is finished
	return 0;

	//~ if(rc > 0) {
	/* rc is the length of the file name in the mem
	   buffer */

	//~ if(attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
	//~ /* this should check what permissions it
	//~ is and print the output accordingly */
	//~ printf("--fix----- ");
	//~ }
	//~ else {
	//~ printf("---------- ");
	//~ }

	//~ if(attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
	//~ printf("%4d %4d ", (int) attrs.uid, (int) attrs.gid);
	//~ }
	//~ else {
	//~ printf("   -	- ");
	//~ }

	//~ if(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
	//~ printf("%8" LIBSSH2_FILESIZE_MASK " ", attrs.filesize);
	//~ }

	//~ printf("%s\n", mem);
	//~ }
	//~ else if(rc == LIBSSH2_ERROR_EAGAIN) {
	//~ /* blocking */
	//~ fprintf(stderr, "Blocking\n");
	//~ }
	//~ else {
	//~ break;
	//~ }
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
