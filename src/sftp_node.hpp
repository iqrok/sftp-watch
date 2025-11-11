#ifndef _SFTP_NODE_HPP
#define _SFTP_NODE_HPP

#include <cstdint>
#include <map>
#include <semaphore>
#include <thread>
#include <vector>

#include <napi.h>

#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#if defined(_POSIX_VERSION)
#	include <netinet/in.h>
#	include <dirent.h>
#elif defined(_WIN32)
#	include <windows.h>
#endif

// default to 30000, as it is the value of max SFTP Packet
#ifndef SFTP_READ_BUFFER_SIZE
#	define SFTP_READ_BUFFER_SIZE 30000
#endif

#ifndef SFTP_FILENAME_MAX_LEN
#	define SFTP_FILENAME_MAX_LEN 512
#endif

#define EVT_STR_DEL "del"
#define EVT_STR_NEW "new"
#define EVT_STR_MOD "mod"
#define EVT_STR_REN "ren"

#define SNOD_FILE_SIZE_SAME(f1, f2)  ((f1).attrs.filesize == (f2).attrs.filesize)
#define SNOD_FILE_MTIME_SAME(f1, f2) (((f1).attrs.mtime == (f2).attrs.mtime))
#define SNOD_FILE_IS_DIFF(f1, f2)                                              \
	(!SNOD_FILE_SIZE_SAME(f1, f2) || !SNOD_FILE_MTIME_SAME(f1, f2))
#define SNOD_FILE_PERM(attr) (attr.permissions & 0777)

#define SNOD_SEP      "/"
#define SNOD_SEP_CHAR SNOD_SEP[0]

#define SNOD_DELAY_US(us)                                                      \
	std::this_thread::sleep_for(std::chrono::microseconds((us)))
#define SNOD_DELAY_MS(ms)                                                      \
	std::this_thread::sleep_for(std::chrono::milliseconds((ms)))
#define SNOD_SEC2MS(s) ((s) * 1000)

#ifndef SNOD_HOSTKEY_HASH
#	define SNOD_HOSTKEY_HASH LIBSSH2_HOSTKEY_HASH_SHA1
#endif

#if ((SNOD_HOSTKEY_HASH) == (LIBSSH2_HOSTKEY_HASH_MD5))
#	define SNOD_FINGERPRINT_LEN 16U
#elif ((SNOD_HOSTKEY_HASH) == (LIBSSH2_HOSTKEY_HASH_SHA1))
#	define SNOD_FINGERPRINT_LEN 20U
#elif ((SNOD_HOSTKEY_HASH) == (LIBSSH2_HOSTKEY_HASH_SHA256))
#	define SNOD_FINGERPRINT_LEN 32U
#else
#	error "SNOD_HOSTKEY_HASH is undefined"
#endif

#define LOG_ERR(...) fprintf(stderr, __VA_ARGS__)

#ifndef NDEBUG
#	define LOG_DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#	define LOG_DBG(...) ((void)0)
#endif

enum FileType_e {
	IS_INVALID  = '0',
	IS_SYMLINK  = 'l',
	IS_REG_FILE = 'f',
	IS_DIR      = 'd',
	IS_CHR_FILE = 'c',
	IS_BLK_FILE = 'b',
	IS_PIPE     = 'p',
	IS_SOCK     = 's',
};

enum EventFile_e {
	EVT_FILE_DEL,
	EVT_FILE_NEW,
	EVT_FILE_MOD,
	EVT_FILE_REN,
};

typedef struct EvtFile_s   EvtFile_t;
typedef struct DirItem_s   DirItem_t;
typedef struct SftpWatch_s SftpWatch_t;
typedef struct Directory_s Directory_t;

typedef std::map<std::string, Directory_t> DirList_t;
typedef std::map<std::string, DirItem_t>   PathFile_t;
typedef std::map<std::string, PathFile_t>  DirSnapshot_t;

struct DirItem_s {
	/** Type of file as stated in #FileType_e */
	uint8_t type = 0;

	/** file name represented as path relative to root path */
	std::string name;

	/** File attributes. Also be used for local directory */
	LIBSSH2_SFTP_ATTRIBUTES attrs;
};

struct Directory_s {
	bool        is_opened = false; /**< Directory open status */
	uint8_t     level     = 0;
	std::string rela;              /**< path relative to root path */
	std::string path;              /**< absoulte path */

	/** SFTP handle for remote directory. not used for local directory */
	LIBSSH2_SFTP_HANDLE* handle = NULL;

#if defined(_POSIX_VERSION)
	/** Directory handle for local directory in POSIX. unused for remote */
	DIR* loc_handle = NULL;
#elif defined(_WIN32)
	HANDLE loc_handle = NULL;
#endif
};

struct SftpWatch_s {
	uint32_t    id;
	int16_t     timeout_sec = 60U;
	uint16_t    port        = 22U;
	std::string host;
	std::string username;
	std::string remote_path;
	std::string local_path;

	std::string pubkey;
	std::string privkey;
	std::string password;
	bool        use_keyboard = true;

	bool    is_connected  = false;
	uint8_t err_count     = 0;
	uint8_t max_err_count = 3;

	libssh2_socket_t     sock;
	LIBSSH2_SESSION*     session;
	LIBSSH2_SFTP*        sftp_session;
	struct sockaddr_in   sin;
	std::vector<uint8_t> fingerprint;

	Napi::Promise::Deferred  deferred;
	std::thread              thread;
	Napi::ThreadSafeFunction tsfn;
	std::binary_semaphore    sem;

	/**< collection of directory that should be iterated */
	DirSnapshot_t base_snap;

	/**< collection of directory that should be iterated */
	DirList_t remote_dirs;

	/** Snapshot per directory. Containing list of files*/
	DirSnapshot_t remote_snap;

	/** List of files that will be downloaded */
	std::vector<DirItem_t*> downloads;

	/** List of directory that should be removed from iteration */
	std::vector<std::string> remote_undirs;

	/**< collection of directory that should be iterated */
	DirList_t local_dirs;

	/** Snapshot per directory. Containing list of files*/
	DirSnapshot_t local_snap;

	/** List of files that will be downloaded */
	std::vector<DirItem_t*> uploads;

	/** List of directory that should be removed from iteration */
	std::vector<std::string> local_undirs;

	/** pointer to event data for js callback */
	EvtFile_t* ev_file;

	bool     is_stopped = false; /**< set to true to stop sync loop */
	uint32_t delay_ms   = 1000;  /**< delay between sync loop */

	SftpWatch_s(Napi::Env env, uint32_t id, std::string host,
		std::string username, std::string pubkey, std::string privkey,
		std::string password, Directory_t remote_dir, Directory_t local_dir)
		: id(id)
		, host(host)
		, username(username)
		, remote_path(remote_dir.path)
		, local_path(local_dir.path)
		, pubkey(pubkey)
		, privkey(privkey)
		, password(password)
		, deferred(Napi::Promise::Deferred::New(env))
		, sem(0) // semaphore is initially locked
		, is_stopped(false)
	{
		this->remote_dirs["/"] = remote_dir;
		this->local_dirs["/"]  = local_dir;
	}
};

#endif
