#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#if defined(_POSIX_VERSION)
#	include <netdb.h>
#	include <sys/socket.h>
#	include <sys/stat.h>
#	include <sys/time.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#	include <unistd.h>
#	include <utime.h>
#elif defined(_WIN32)
#	include <winsock2.h>  // sockets, basic networking
#	include <ws2tcpip.h>  // getaddrinfo, inet_pton, etc.
#	include <windows.h>   // general Windows API, timeval replacement
#	include <io.h>        // _close, _read, _write (instead of unistd.h)
#	include <sys/types.h> // basic types
#	include <sys/stat.h>  // file status
#	include <sys/utime.h> // _utime(), _utimbuf

#	define write(f, b, c) write((f), (b), (unsigned int)(c))

// TODO: Needs to check these on Windows
#	define stat           _stat
#	define S_ISDIR(mode)  ((mode) & _S_IFDIR)
#elif
#	error "UNKNOWN ENVIRONMENT"
#endif

#include "sftp_helper.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <filesystem> // for removing directory

static bool is_inited = false;

#define FN_RC_EAGAIN(rc, fn)   (((rc) = (fn)) == LIBSSH2_ERROR_EAGAIN)
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

static void kbd_callback(const char* name, int name_len,
	const char* instruction, int instruction_len, int num_prompts,
	const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
	LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract)
{
	(void)name;
	(void)name_len;
	(void)instruction;
	(void)instruction_len;
	(void)prompts;

	// only expects 1 prompt, which is the password for user
	if (num_prompts != 1) return;

	SftpWatch_t* ctx = static_cast<SftpWatch_t*>(*abstract);

	responses[0].text   = strdup(ctx->password.c_str());
	responses[0].length = ctx->password.size();
}

static int32_t prv_auth_password(SftpWatch_t* ctx)
{
	int32_t rc = LIBSSH2_ERROR_EAGAIN;

	do {
		if (ctx->use_keyboard) {
			rc = libssh2_userauth_keyboard_interactive_ex(ctx->session,
				ctx->username.c_str(), ctx->username.size(), &kbd_callback);
		} else {
			rc = libssh2_userauth_password(
				ctx->session, ctx->username.c_str(), ctx->password.c_str());
		}
	} while (rc == LIBSSH2_ERROR_EAGAIN);

	if (rc) {
		fprintf(stderr, "Authentication by password failed %d [%s].\n", rc,
			ctx->username.c_str());
	}

	return rc;
}

static LIBSSH2_SFTP_HANDLE* prv_open_file(
	SftpWatch_t* ctx, const char* remote_path)
{
	LIBSSH2_SFTP_HANDLE* handle = NULL;

	// try to open remote file and wait until socket ready
	do {
		handle = libssh2_sftp_open(
			ctx->sftp_session, remote_path, LIBSSH2_FXF_READ, 0);

		if (!handle) {
			if (FN_LAST_ERRNO_ERROR(ctx->session)) {
				fprintf(stderr, "Unable to open file '%s' with SFTP: %ld\n",
					remote_path, libssh2_sftp_last_error(ctx->sftp_session));
				return NULL;
			} else {
				// non-blocking open, now we wait until socket is ready
				waitsocket(ctx->sock, ctx->session);
			}
		}
	} while (!handle);

	if (!handle) {
		fprintf(stderr, "Unable to open file '%s' with SFTP: %ld\n",
			remote_path, libssh2_sftp_last_error(ctx->sftp_session));
		return NULL;
	}

	return handle;
}

int32_t SftpHelper::connect(SftpWatch_t* ctx)
{
	int32_t rc;

	if (!is_inited) {
#ifdef _WIN32
		WSADATA wsadata;

		rc = WSAStartup(MAKEWORD(2, 0), &wsadata);
		if (rc) {
			fprintf(stderr, "WSAStartup failed with error: %d\n", rc);
			return 1;
		}
#endif
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

	// NOTE: always forgot this. FREE getaddrinfo res AFTER USE
	freeaddrinfo(res);

	/* Create a session instance
	 * using extended API to set SftpWatch_t context as abstract. This way we
	 * can access the context from callback
	 * */
	ctx->session = libssh2_session_init_ex(NULL, NULL, NULL, ctx);
	if (!ctx->session) {
		fprintf(stderr, "Could not initialize SSH session.\n");
		return -1;
	}

	/* Since we have set non-blocking, tell libssh2 we are non-blocking */
	libssh2_session_set_blocking(ctx->session, 0);
	libssh2_session_set_timeout(ctx->session, ctx->timeout_sec);

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
	int32_t rc;

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
			char*   errmsg;
			int32_t errcode
				= libssh2_session_last_error(ctx->session, &errmsg, NULL, 0);

			fprintf(stderr, "Authentication by public key failed [%d] %s\n",
				errcode, errmsg ? errmsg : "Unknown error message");
			return -1;
		}
	} else if (!ctx->password.empty()) {
		rc = prv_auth_password(ctx);
		if (rc) {
			fprintf(stderr, "Authentication by password failed %d [%s].\n", rc,
				ctx->username.c_str());
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

int32_t SftpHelper::close_dir(SftpWatch_t* ctx, RemoteDir_t* dir)
{
	if (!dir->is_opened) return 0;

	int32_t rc = 0;

	while (FN_RC_EAGAIN(rc, libssh2_sftp_closedir(dir->handle)));

	if (rc) {
		int32_t errcode = libssh2_sftp_last_error(ctx->sftp_session);

		fprintf(stderr, "Failed to close dir '%s' [%d]\n", dir->path.c_str(),
			errcode);

		return errcode;
	}

	dir->is_opened = false;

	return 0;
}

int32_t SftpHelper::open_dir(SftpWatch_t* ctx, RemoteDir_t* dir)
{
	if (dir->is_opened) SftpHelper::close_dir(ctx, dir);

	do {
		dir->handle
			= libssh2_sftp_opendir(ctx->sftp_session, dir->path.c_str());

		if (!dir->handle && FN_LAST_ERRNO_ERROR(ctx->session)) {
			char*   errmsg;
			int32_t errcode
				= libssh2_session_last_error(ctx->session, &errmsg, NULL, 0);

			fprintf(stderr,
				"Unable to open dir '%s' '%s' with SFTP [%d] %s %s:%d\n",
				dir->path.c_str(), dir->rela.c_str(), errcode,
				errmsg ? errmsg : "Unknown error message", __FILE__, __LINE__);

			return errcode;
		}
	} while (!dir->handle);

	dir->is_opened = true;

	return 0;
}

int32_t SftpHelper::read_dir(RemoteDir_t& dir, DirItem_t* file)
{
	int32_t rc = 0;

	char filename[SFTP_FILENAME_MAX_LEN];
	while ((rc = libssh2_sftp_readdir(
				dir.handle, filename, sizeof(filename), &file->attrs))
		== LIBSSH2_ERROR_EAGAIN);

	// there's a record
	if (rc > 0) {
		std::string name(filename);

		if (name == "." || name == "..") {
			file->name           = "";
			file->type           = IS_INVALID;
			file->attrs.filesize = 0;
			file->attrs.mtime    = 0;
			return 1;
		}

		file->type = SftpHelper::get_filetype(file);
		file->name = dir.rela.empty() ? name : dir.rela + SNOD_SEP + name;

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

void SftpHelper::disconnect(SftpWatch_t* ctx)
{
	libssh2_sftp_shutdown(ctx->sftp_session);

	if (ctx->session) {
		libssh2_session_disconnect(ctx->session, "Normal Shutdown");
		libssh2_session_free(ctx->session);
	}

	if (ctx->sock != LIBSSH2_INVALID_SOCKET) {
		// NOTE: this is how to prevent name conflict with extern "C"
		::shutdown(ctx->sock, SHUT_RDWR);
		LIBSSH2_SOCKET_CLOSE(ctx->sock);
	}
}

void SftpHelper::shutdown()
{
	libssh2_exit();

#ifdef _WIN32
	WSACleanup();
#endif

	is_inited = false;
}

int32_t SftpHelper::copy_symlink_remote(SftpWatch_t* ctx, DirItem_t* file)
{
	std::string remote_file = ctx->remote_path + SNOD_SEP + file->name;
	std::string local_file  = ctx->local_path + SNOD_SEP + file->name;
	struct stat st;

	int32_t rc     = 0;
	char mem[4096] = { 0 };

	while(FN_RC_EAGAIN(rc, libssh2_sftp_readlink(
		ctx->sftp_session, remote_file.c_str(), mem, sizeof(mem))));

	if (rc < 0) {
		fprintf(stderr, "Unable to open file '%s' with SFTP: %ld\n",
			remote_file.c_str(), libssh2_sftp_last_error(ctx->sftp_session));
		return rc;
	}

	// check if symlonk exists and remove it
	if ((rc = lstat(local_file.c_str(), &st)) == 0) {
		if (S_ISLNK(st.st_mode)) {
			SftpHelper::remove_local(ctx, file->name.c_str());
		}
	}

	if ((rc = symlink(mem, local_file.c_str()))) {
		fprintf(stderr, "Failed to create symlink '%s' with SFTP: %d\n",
			local_file.c_str(), rc);
		perror("SYMLINK FAILED");
	}

	return rc;
}

int32_t SftpHelper::copy_file_remote(SftpWatch_t* ctx, DirItem_t* file)
{
	std::string remote_file = ctx->remote_path + SNOD_SEP + file->name;
	std::string local_file  = ctx->local_path + SNOD_SEP + file->name;

	LIBSSH2_SFTP_HANDLE* handle = prv_open_file(ctx, remote_file.c_str());

	int32_t err      = 0;
	FILE*   fd_local = fopen(local_file.c_str(), "wb");

	if (!fd_local) {
		fprintf(stderr, "Error opening file!\n");
		return -2;
	}

	// connection loop, check if socket is ready
	do {
		int32_t nread = 0;

		// remote read loop, loop until failed or no remaining bytes
		do {
			char mem[SFTP_READ_BUFFER_SIZE];

			nread = libssh2_sftp_read(handle, mem, sizeof(mem));
			if (nread > 0) fwrite(mem, (size_t)nread, 1, fd_local);

		} while (nread > 0);

		// error or end of file
		if (nread != LIBSSH2_ERROR_EAGAIN) break;

		// this block is to wait for socket to be ready. Copied from example
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

			// -3 is timeout, -1 is error.
			// FIXME: Should use enum instead this magic number
			err = rc == 0 ? -3 : -1;
			break;
		}
	} while (1);

	// close both sftp and file handle
	libssh2_sftp_close(handle);
	fclose(fd_local);

	// set modification time for local file to match the remote one
	// this must be done AFTER CLOSING the file handle
	struct utimbuf times = {
		.actime  = (time_t)file->attrs.atime,
		.modtime = (time_t)file->attrs.mtime,
	};

	// set modified & access time time to match remote
	if (utime(local_file.c_str(), &times)) {
		fprintf(stderr, "Failed to set mtime [%d]\n", errno);
	}

#ifdef _POSIX_VERSION
	// set file attribute to match remote. non-windows only
	if (chmod(local_file.c_str(), SNOD_FILE_PERM(file->attrs))) {
		fprintf(stderr, "Failed to set attributes: %d\n", errno);
	}
#endif

	return err;
}

int32_t SftpHelper::remove_local(SftpWatch_t* ctx, std::string filename)
{
	std::string local_file = ctx->local_path + SNOD_SEP + filename;

	if (remove(local_file.c_str())) {
		fprintf(stderr, "Err %d: %s '%s'\n", errno, strerror(errno),
			local_file.c_str());
		return -1;
	}

	return 0;
}

int32_t SftpHelper::mkdir_local(SftpWatch_t* ctx, DirItem_t* file)
{
	std::string local_dir = ctx->local_path + SNOD_SEP + file->name;
	struct stat st;
	int32_t     rc = 0;

	struct utimbuf times = {
		.actime  = (time_t)file->attrs.atime,
		.modtime = (time_t)file->attrs.mtime,
	};

	// check if path exists
	if (stat(local_dir.c_str(), &st) == 0) {
		// existing path is directory. Return success
		if (S_ISDIR(st.st_mode)) {
			// set modified & access time time to match remote
			utime(local_dir.c_str(), &times);
			return 0;
		}

		// TODO: what to do when path exist but not a directory
		return -1;
	}

	// create directory if doesn't exist
#ifdef _POSIX_VERSION
	rc = mkdir(local_dir.c_str(), SNOD_FILE_PERM(file->attrs));
	if (rc) {
		fprintf(stderr, "Failed create directory: %d\n", errno);
		return rc;
	}

	// set modified & access time time to match remote
	if (utime(local_dir.c_str(), &times)) {
		fprintf(stderr, "Failed to set mtime [%d]\n", errno);
	}
#endif

#ifdef _WIN32
	// NOTE: If the CreateDirectoryA succeeds, the return value is nonzero.
	rc = CreateDirectoryA(local_dir.c_str(), NULL);
	if (!rc) fprintf(stderr, "Failed create directory: %d\n", GetLastError());
#endif

	return rc;
}

/*
 * NOTE: using C++ filesystem to remove directory and its contents
 *       ref https://stackoverflow.com/a/50051546/3258981
 * */
void SftpHelper::rmdir_local(SftpWatch_t* ctx, std::string dirname)
{
	std::filesystem::path dirpath(ctx->local_path + SNOD_SEP + dirname);

	if (!std::filesystem::is_directory(dirpath)) return;

	std::filesystem::remove_all(dirpath);
}
