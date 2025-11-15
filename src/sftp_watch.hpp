#ifndef _SFTP_NODE_HPP
#define _SFTP_NODE_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <semaphore>
#include <thread>
#include <unordered_set>
#include <vector>

#include <libssh2.h>
#include <libssh2_sftp.h>

#if defined(_POSIX_VERSION)
#	include <netinet/in.h>
#	include <dirent.h>
#elif defined(_WIN32)
#	include <windows.h>
#endif

#ifndef NDEBUG
/** Alias for SIGTRAP in debug mode. */
#	define __breakpoint() asm volatile("int $3")
#else
#	define __breakpoint()
#endif

// default to 30000, as it is the value of max SFTP Packet
#ifndef SFTP_READ_BUFFER_SIZE
#	define SFTP_READ_BUFFER_SIZE 30000
#endif

#ifndef SFTP_FILENAME_MAX_LEN
#	define SFTP_FILENAME_MAX_LEN 512
#endif

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

#ifndef _WIN32
#	define SNOD_SEC2MS(s) ((s) * 1000)
#else
// cast to double, avoiding overflow value
#	define SNOD_SEC2MS(s) ((double)(s) * 1000.0)
#endif

#define SNOD_CHR2STR(s) (std::string(1, (s)))

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

typedef enum EventFile_e {
	EVT_FILE_LDEL = 0x00,
	EVT_FILE_UP   = 0x01,
	EVT_FILE_RDEL = 0x02,
	EVT_FILE_DOWN = 0x03,
} EventFile_t;

typedef void* UserData_t;

typedef struct DirItem_s   DirItem_t;
typedef struct SftpWatch_s SftpWatch_t;
typedef struct Directory_s Directory_t;
typedef struct SyncQueue_s SyncQueue_t;

typedef std::map<std::string, Directory_t> DirList_t;
typedef std::map<std::string, DirItem_t>   PathFile_t;
typedef std::map<std::string, PathFile_t>  DirSnapshot_t;

typedef std::map<std::string, std::unordered_set<std::string>> AllIns_t;

typedef void (*sync_file_cb)(SftpWatch_t* ctx, UserData_t data, DirItem_t* file,
	bool status, EventFile_t ev);
typedef void (*sync_cleanup_cb)(SftpWatch_t* ctx, UserData_t data);

struct SyncQueue_s {
	std::vector<DirItem_t*> l_new;
	std::vector<DirItem_t*> r_new;
	std::vector<DirItem_t>  r_del;
	std::vector<DirItem_t>  l_del;
};

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
	uint8_t     depth     = 0;
	std::string rela;              /**< path relative to root path */
	std::string path;              /**< absoulte path */

	/** SFTP handle for remote directory. not used for local directory */
	LIBSSH2_SFTP_HANDLE* handle = NULL;

#if defined(_POSIX_VERSION)
	/** Directory handle for local directory in POSIX. unused for remote */
	DIR* loc_handle = NULL;
#elif defined(_WIN32)
	/** Directory handle for local directory in WIN32. unused for remote */
	HANDLE loc_handle = NULL;
#endif
};

struct SftpWatch_s {
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

	std::atomic<bool>    is_connected  = false;
	std::atomic<uint8_t> err_count     = 0;
	uint8_t              max_err_count = 3;

	std::atomic<bool> is_stopped = false; /**< set to true to stop sync loop */
	uint32_t          delay_ms   = 1000;  /**< delay between sync loop */

	libssh2_socket_t     sock;
	LIBSSH2_SESSION*     session;
	LIBSSH2_SFTP*        sftp_session;
	std::vector<uint8_t> fingerprint;

	std::thread thread;

	/** Snapshots */
	DirSnapshot_t base_snap;
	DirSnapshot_t remote_snap;
	DirSnapshot_t local_snap;

	/** collection of directory that should be iterated */
	DirList_t remote_dirs;
	DirList_t local_dirs;

	sync_file_cb    cb_file;
	sync_cleanup_cb cb_cleanup;

	UserData_t user_data = nullptr;

	SftpWatch_s(std::string host, std::string username, std::string pubkey,
		std::string privkey, std::string password, Directory_t remote_dir,
		Directory_t local_dir, sync_file_cb cb_file, sync_cleanup_cb cb_cleanup)
		: host(host)
		, username(username)
		, remote_path(remote_dir.path)
		, local_path(local_dir.path)
		, pubkey(pubkey)
		, privkey(privkey)
		, password(password)
		, cb_file(cb_file)
		, cb_cleanup(cb_cleanup)
	{
		this->remote_dirs[SNOD_SEP] = remote_dir;
		this->local_dirs[SNOD_SEP]  = local_dir;
	}
};

namespace SftpWatch {

void    disconnect(SftpWatch_t* ctx);
int32_t connect_or_reconnect(SftpWatch_t* ctx);
int32_t set_user_data(SftpWatch_t* ctx, UserData_t data);
//~ void    sync_thread(SftpWatch_t* ctx);
void    start(SftpWatch_t* ctx);
void    clear(SftpWatch_t* ctx);

}

#endif
