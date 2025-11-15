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

#	define SHUT_RDWR     SD_BOTH
#	define stat          _stat
#	define S_ISDIR(mode) ((mode) & _S_IFDIR)
#elif
#	error "UNKNOWN ENVIRONMENT"
#endif

#include "sftp_local.hpp"
#include "sftp_remote.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <filesystem> // for removing directory

#define FN_RC_EAGAIN(rc, fn)   (((rc) = (fn)) == LIBSSH2_ERROR_EAGAIN)
#define FN_ACTUAL_ERROR(err)   ((err) != LIBSSH2_ERROR_EAGAIN)
#define FN_LAST_ERRNO_ERROR(s) FN_ACTUAL_ERROR(libssh2_session_last_errno(s))

#define SNOD_WAIT_STABLE 250

#ifdef LOG_DBG
#	define LOG_DBG_FINGERPRINT(fp)                                            \
		do {                                                                   \
			LOG_ERR("Fingerprint ");                                           \
			for (uint8_t fp_byte : fp) LOG_ERR(":%02X", fp_byte);              \
			LOG_ERR("\n");                                                     \
		} while (0)
#endif

namespace { // start of unnamed namespace for static function

enum RemoteOpenDirection_e {
	SNOD_REMOTE_OPEN_READ = LIBSSH2_FXF_READ,
	SNOD_REMOTE_OPEN_WRITE
	= (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC),
};

static bool is_inited = false; /**< Whether libssh2 is initialized or nor */

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
		LOG_ERR("Authentication by password failed %d [%s].\n", rc,
			ctx->username.c_str());
	}

	return rc;
}

static LIBSSH2_SFTP_HANDLE* prv_open_file(
	SftpWatch_t* ctx, const char* remote_path, uint8_t direction, uint32_t mode)
{
	LIBSSH2_SFTP_HANDLE* handle = NULL;

	// try to open remote file and wait until socket ready
	do {
		handle = libssh2_sftp_open(
			ctx->sftp_session, remote_path, direction, (long)mode);

		if (!handle) {
			if (FN_LAST_ERRNO_ERROR(ctx->session)) {
				LOG_ERR("Unable to open file '%s' with SFTP: %ld\n",
					remote_path, libssh2_sftp_last_error(ctx->sftp_session));
				break;
			} else {
				// non-blocking open, now we wait until socket is ready
				waitsocket(ctx->sock, ctx->session);
			}
		}
	} while (!handle);

	if (!handle) {
		LOG_ERR("Unable to open file '%s' with SFTP: %ld\n", remote_path,
			libssh2_sftp_last_error(ctx->sftp_session));
		return NULL;
	}

	return handle;
}

} // end of unnamed namespace for static function

int32_t SftpRemote::connect(SftpWatch_t* ctx)
{
	int32_t rc;

	if (!is_inited) {
#ifdef _WIN32
		WSADATA wsadata;

		rc = WSAStartup(MAKEWORD(2, 0), &wsadata);
		if (rc) {
			LOG_ERR("WSAStartup failed with error: %d\n", rc);
			return 1;
		}
#endif
		if ((rc = libssh2_init(0)) != 0) {
			LOG_ERR("libssh2 initialization failed (%d)\n", rc);
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
		LOG_ERR("FAILED getaddrinfo %d %d\n", rc, errno);
		if (res) freeaddrinfo(res);

		return -1;
	}

	ctx->sock = socket(res->ai_family, res->ai_socktype, 0);
	if (ctx->sock == LIBSSH2_INVALID_SOCKET) {
		LOG_ERR("failed to create socket.\n");
		freeaddrinfo(res);

		return -1;
	}

	if ((rc = connect(ctx->sock, res->ai_addr, res->ai_addrlen))) {
		LOG_ERR("failed to connect. (%d) [%s:%u]\n", rc, ctx->host.c_str(),
			ctx->port);
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
		LOG_ERR("Could not initialize SSH session.\n");
		return -1;
	}

	/* Since we have set non-blocking, tell libssh2 we are non-blocking */
	libssh2_session_set_blocking(ctx->session, 0);
	libssh2_session_set_timeout(ctx->session, ctx->timeout_sec);
	libssh2_session_flag(ctx->session, LIBSSH2_FLAG_COMPRESS, 1);

	while (
		FN_RC_EAGAIN(rc, libssh2_session_handshake(ctx->session, ctx->sock)));

	if (rc) {
		LOG_ERR("Failure establishing SSH session: %d\n", rc);
		return -1;
	}

	const char* fp;
	if ((fp = libssh2_hostkey_hash(ctx->session, SNOD_HOSTKEY_HASH))) {
		ctx->fingerprint = std::vector<uint8_t>(fp, fp + SNOD_FINGERPRINT_LEN);
		LOG_DBG_FINGERPRINT(ctx->fingerprint);
	}

	return 0;
}

int32_t SftpRemote::auth(SftpWatch_t* ctx)
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

			LOG_ERR("Authentication by public key failed [%d] %s\n", errcode,
				errmsg ? errmsg : "Unknown error message");
			return -1;
		}
	} else if (!ctx->password.empty()) {
		rc = prv_auth_password(ctx);
		if (rc) {
			LOG_ERR("Authentication by password failed %d [%s].\n", rc,
				ctx->username.c_str());
			return -1;
		}
	} else {
		LOG_ERR("No Valid Authentication is provided.\n");
		return -2;
	}

	do {
		ctx->sftp_session = libssh2_sftp_init(ctx->session);

		if (!ctx->sftp_session && FN_LAST_ERRNO_ERROR(ctx->session)) {
			LOG_ERR("Unable to init SFTP session\n");
			return -3;
		}
	} while (!ctx->sftp_session);

	return 0;
}

int32_t SftpRemote::close_dir(SftpWatch_t* ctx, Directory_t* dir)
{
	if (!dir->is_opened) return 0;

	int32_t rc = 0;

	while (FN_RC_EAGAIN(rc, libssh2_sftp_closedir(dir->handle)));

	if (rc) {
		int32_t errcode = libssh2_sftp_last_error(ctx->sftp_session);

		LOG_ERR("Failed to close dir '%s' [%d]\n", dir->path.c_str(), errcode);

		return errcode;
	}

	dir->is_opened = false;

	return 0;
}

int32_t SftpRemote::open_dir(SftpWatch_t* ctx, Directory_t* dir)
{
	if (dir->is_opened) SftpRemote::close_dir(ctx, dir);

	do {
		dir->handle
			= libssh2_sftp_opendir(ctx->sftp_session, dir->path.c_str());

		if (!dir->handle && FN_LAST_ERRNO_ERROR(ctx->session)) {
			char*   errmsg;
			int32_t errcode
				= libssh2_session_last_error(ctx->session, &errmsg, NULL, 0);

			LOG_ERR("Unable to open dir '%s' '%s' with SFTP [%d] %s\n",
				dir->path.c_str(), dir->rela.c_str(), errcode,
				errmsg ? errmsg : "Unknown error message");

			// return the errno detail from sftp session
			return libssh2_sftp_last_error(ctx->sftp_session);
		}
	} while (!dir->handle);

	dir->is_opened = true;

	return 0;
}

int32_t SftpRemote::read_dir(Directory_t& dir, DirItem_t* file)
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

		file->type = SftpRemote::get_filetype(file);
		file->name = dir.rela.empty() ? name : dir.rela + SNOD_SEP + name;

		return 1;
	}

	// should try again
	if (rc == LIBSSH2_ERROR_EAGAIN) return 2;

	// read dir is finished
	return 0;
}

uint8_t SftpRemote::get_filetype(DirItem_t* file)
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

void SftpRemote::disconnect(SftpWatch_t* ctx)
{
	int32_t rc = 0;

	while (FN_RC_EAGAIN(rc, libssh2_sftp_shutdown(ctx->sftp_session))) {
		waitsocket(ctx->sock, ctx->session);
	}

	if (ctx->session) {
		while (FN_RC_EAGAIN(
			rc, libssh2_session_disconnect(ctx->session, "Normal Shutdown"))) {
			waitsocket(ctx->sock, ctx->session);
		}

		while (FN_RC_EAGAIN(rc, libssh2_session_free(ctx->session))) {
			waitsocket(ctx->sock, ctx->session);
		}
	}

	if (ctx->sock != LIBSSH2_INVALID_SOCKET) {
		// NOTE: this is how to prevent name conflict with extern "C"
		::shutdown(ctx->sock, SHUT_RDWR);
		LIBSSH2_SOCKET_CLOSE(ctx->sock);
	}
}

void SftpRemote::shutdown()
{
	libssh2_exit();

#ifdef _WIN32
	WSACleanup();
#endif

	is_inited = false;
}

int32_t SftpRemote::down_symlink(SftpWatch_t* ctx, DirItem_t* file)
{
#ifdef _WIN32
	// FIXME: symlink in windows
	return SftpRemote::down_file(ctx, file);
#else
	std::string remote_file = ctx->remote_path + SNOD_SEP + file->name;
	std::string local_file  = ctx->local_path + SNOD_SEP + file->name;
	struct stat st;

	int32_t rc        = 0;
	char    mem[4096] = { 0 };

	while (FN_RC_EAGAIN(rc,
		libssh2_sftp_readlink(
			ctx->sftp_session, remote_file.c_str(), mem, sizeof(mem))));

	if (rc < 0) {
		LOG_ERR("Unable to open file '%s' with SFTP: %ld\n",
			remote_file.c_str(), libssh2_sftp_last_error(ctx->sftp_session));
		return rc;
	}

	// check if symlonk exists and remove it
	if ((rc = lstat(local_file.c_str(), &st)) == 0) {
		if (S_ISLNK(st.st_mode)) {
			SftpLocal::remove(ctx, file->name);
		}
	}

	if ((rc = symlink(mem, local_file.c_str()))) {
		LOG_ERR("Failed to create symlink '%s' with SFTP: %d\n",
			local_file.c_str(), rc);
		perror("SYMLINK FAILED");
	}

	return rc;
#endif
}

int32_t SftpRemote::up_file(SftpWatch_t* ctx, DirItem_t* file)
{
	int32_t rc = 0;

	std::string remote_file = ctx->remote_path + SNOD_SEP + file->name;
	std::string local_file  = ctx->local_path + SNOD_SEP + file->name;

	/*
	 * FIXME: How to wait until file is stable in non-blocking way?
	 * */
	LIBSSH2_SFTP_ATTRIBUTES attrs;
	bool                    is_stable = false;
	SftpLocal::filestat(ctx, local_file, &attrs);
	while (!is_stable) {
		SNOD_DELAY_MS(SNOD_WAIT_STABLE);

		SftpLocal::filestat(ctx, local_file, &attrs);
		is_stable = file->attrs.filesize == attrs.filesize;

		memcpy(&file->attrs, &attrs, sizeof(attrs));
	}

	FILE* fd_local = fopen(local_file.c_str(), "rb");

	if (!fd_local) {
		LOG_ERR("Error opening file!\n");
		return -2;
	}

	LIBSSH2_SFTP_HANDLE* handle = prv_open_file(ctx, remote_file.c_str(),
		SNOD_REMOTE_OPEN_WRITE, SNOD_FILE_PERM(file->attrs));

	// connection loop, check if socket is ready
	int32_t nwritten = 0;
	do {
		char* ptr;
		char  mem[SFTP_READ_BUFFER_SIZE];

		int32_t nread = fread(mem, 1, SFTP_READ_BUFFER_SIZE, fd_local);
		if (nread <= 0) {
			break;
		}

		rc  = 0;
		ptr = mem;

		// write to remote untill all read bytes are written
		do {
			while (FN_RC_EAGAIN(
				nwritten, libssh2_sftp_write(handle, ptr, nread))) {
				// negative is error, 0 is timeout
				int32_t wait_rc = waitsocket(ctx->sock, ctx->session);
				if (wait_rc == 0) {
					rc = LIBSSH2_ERROR_TIMEOUT;
					LOG_ERR("SFTP upload timed out: %d\n", rc);
					break;
				} else if (wait_rc < 0) {
					rc = errno;
					LOG_ERR("SFTP upload error: %d\n", rc);
					break;
				} else {
					// no error. continue
					rc = 0;
				}
			}

			if (nwritten < 0) break;

			ptr = &mem[nwritten];
			nread -= nwritten;
		} while (nread > 0 && !rc);
	} while (nwritten > 0);

	// close both sftp and file handle
	while (FN_RC_EAGAIN(rc, libssh2_sftp_close(handle))) {
		waitsocket(ctx->sock, ctx->session);
	}
	fclose(fd_local);

	SftpRemote::set_filestat(ctx, remote_file, &file->attrs);

	return rc;
}

int32_t SftpRemote::down_file(SftpWatch_t* ctx, DirItem_t* file)
{
	int32_t rc = 0;

	std::string remote_file = ctx->remote_path + SNOD_SEP + file->name;
	std::string local_file  = ctx->local_path + SNOD_SEP + file->name;

	/*
	 * FIXME: How to wait until file is stable in non-blocking way?
	 * */
	LIBSSH2_SFTP_ATTRIBUTES attrs;
	bool                    is_stable = false;
	SftpRemote::get_filestat(ctx, remote_file, &attrs);
	while (!is_stable) {
		SNOD_DELAY_MS(SNOD_WAIT_STABLE);

		SftpRemote::get_filestat(ctx, remote_file, &attrs);
		is_stable = file->attrs.filesize == attrs.filesize;

		memcpy(&file->attrs, &attrs, sizeof(attrs));
	}

	FILE* fd_local = fopen(local_file.c_str(), "wb");

	if (!fd_local) {
		LOG_ERR("Error opening file!\n");
		return -2;
	}

	LIBSSH2_SFTP_HANDLE* handle
		= prv_open_file(ctx, remote_file.c_str(), SNOD_REMOTE_OPEN_READ, 0);

	// connection loop, check if socket is ready
	while (handle) {
		int32_t nread = 0;

		// remote read loop, loop until failed or no remaining bytes
		do {
			char mem[SFTP_READ_BUFFER_SIZE];
			nread = libssh2_sftp_read(handle, mem, sizeof(mem));
			if (nread > 0) fwrite(mem, (size_t)nread, 1, fd_local);
		} while (nread > 0);

		// error or end of file
		if (nread != LIBSSH2_ERROR_EAGAIN) {
			if (nread < 0) rc = nread;
			break;
		}

		// negative is error, 0 is timeout
		int32_t wait_rc = waitsocket(ctx->sock, ctx->session);
		if (wait_rc == 0) {
			rc = LIBSSH2_ERROR_TIMEOUT;
			LOG_ERR("SFTP download timed out: %d\n", rc);
			break;
		} else if (wait_rc < 0) {
			rc = errno;
			LOG_ERR("SFTP download error: %d\n", rc);
			break;
		} else {
			// no error. continue
		}
	}

	// close both sftp and file handle
	while (FN_RC_EAGAIN(rc, libssh2_sftp_close(handle))) {
		waitsocket(ctx->sock, ctx->session);
	}

	fclose(fd_local);

	// return now if error
	if (rc) return rc;

	// set modification time for local file to match the remote one
	// this must be done AFTER CLOSING the file handle
	struct utimbuf times = {
		.actime  = (time_t)file->attrs.atime,
		.modtime = (time_t)file->attrs.mtime,
	};

	// set modified & access time time to match remote
	if (utime(local_file.c_str(), &times)) {
		LOG_ERR("Failed to set mtime [%d]\n", errno);
	}

#ifdef _POSIX_VERSION
	// set file attribute to match remote. non-windows only
	if (chmod(local_file.c_str(), SNOD_FILE_PERM(file->attrs))) {
		LOG_ERR("Failed to set attributes: %d\n", errno);
	}
#endif

	return rc;
}

int32_t SftpRemote::remove(SftpWatch_t* ctx, DirItem_t* file)
{
	int32_t rc = 0;

	std::string remote_file = ctx->remote_path + SNOD_SEP + file->name;

	while (FN_RC_EAGAIN(
		rc, libssh2_sftp_unlink(ctx->sftp_session, remote_file.c_str())));

	return rc;
}

int32_t SftpRemote::mkdir(SftpWatch_t* ctx, DirItem_t* dir)
{
	int32_t rc = 0;

	std::string remote_dir = ctx->remote_path + SNOD_SEP + dir->name;

	long mode = SNOD_FILE_PERM(dir->attrs);
	while (FN_RC_EAGAIN(
		rc, libssh2_sftp_mkdir(ctx->sftp_session, remote_dir.c_str(), mode)));

	SftpRemote::set_filestat(ctx, remote_dir, &dir->attrs);

	return rc;
}

int32_t SftpRemote::rmdir(SftpWatch_t* ctx, DirItem_t* dir)
{
	int32_t rc = 0;

	std::string remote_dir = ctx->remote_path + SNOD_SEP + dir->name;

	Directory_t target;
	target.rela = dir->name;
	target.path = remote_dir;

	// open remote dir first
	if ((rc = SftpRemote::open_dir(ctx, &target))) return rc;

	DirItem_t item;
	while ((rc = SftpRemote::read_dir(target, &item))) {
		if (item.name.empty()) continue;

		if (item.type == IS_DIR) {
			DirItem_t subdir;
			subdir.name = item.name;
			SftpRemote::rmdir(ctx, &subdir);
		} else {
			SftpRemote::remove(ctx, &item);
		}
	}

	SftpRemote::close_dir(ctx, &target);

	while (FN_RC_EAGAIN(
		rc, libssh2_sftp_rmdir(ctx->sftp_session, remote_dir.c_str())));

	return rc;
}

int32_t SftpRemote::set_filestat(
	SftpWatch_t* ctx, std::string& path, LIBSSH2_SFTP_ATTRIBUTES* attrs)
{
	int32_t rc = 0;

	// create copy to preserve original in case failure happens
	LIBSSH2_SFTP_ATTRIBUTES remote_attrs = *attrs;

	while (FN_RC_EAGAIN(rc,
		libssh2_sftp_stat_ex(ctx->sftp_session, path.c_str(), path.size(),
			LIBSSH2_SFTP_SETSTAT, &remote_attrs))) {
		if (waitsocket(ctx->sock, ctx->session) < 0) break;
	}

	return rc;
}

int32_t SftpRemote::get_filestat(
	SftpWatch_t* ctx, std::string& path, LIBSSH2_SFTP_ATTRIBUTES* attrs)
{
	int32_t rc = 0;

	while (FN_RC_EAGAIN(rc,
		libssh2_sftp_stat_ex(ctx->sftp_session, path.c_str(), path.size(),
			LIBSSH2_SFTP_LSTAT, attrs))) {
		if (waitsocket(ctx->sock, ctx->session) < 0) break;
	}

	return rc;
}
