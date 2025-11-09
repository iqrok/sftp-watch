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

#ifdef _POSIX_VERSION
#	include <netinet/in.h>
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
#define SNOD_FILE_PERM(attr)         (attr.permissions & 0777)

#define SNOD_SEP      "/"
#define SNOD_SEP_CHAR SNOD_SEP[0]

#define SNOD_DELAY_US(us)                                                      \
	std::this_thread::sleep_for(std::chrono::microseconds((us)))
#define SNOD_DELAY_MS(ms)                                                      \
	std::this_thread::sleep_for(std::chrono::milliseconds((ms)))
#define SNOD_SEC2MS(s) ((s) * 1000)

#define LOG_ERR(...) fprintf(stderr, __VA_ARGS__)

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
typedef struct RemoteDir_s RemoteDir_t;

typedef std::map<std::string, DirItem_t> PairFileDet_t;

struct DirItem_s {
	uint8_t     type = 0;
	std::string name;

	LIBSSH2_SFTP_ATTRIBUTES attrs;
};

struct RemoteDir_s {
	bool        is_opened = false;
	std::string rela;
	std::string path;

	LIBSSH2_SFTP_HANDLE* handle;
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

	libssh2_socket_t   sock;
	LIBSSH2_SESSION*   session;
	LIBSSH2_SFTP*      sftp_session;
	struct sockaddr_in sin;
	const char*        fingerprint;

	Napi::Promise::Deferred  deferred;
	std::thread              thread;
	Napi::ThreadSafeFunction tsfn;
	std::binary_semaphore    sem;

	// collection of directory that should be iterated
	std::map<std::string, RemoteDir_t>   dirs;
	std::map<std::string, PairFileDet_t> last_files;
	std::vector<std::string>             undirs;
	std::vector<DirItem_t*>              downloads;

	// pointer to event data for js callback
	EvtFile_t* ev_file;

	bool     is_stopped;
	uint32_t delay_ms = 1000;

	SftpWatch_s(Napi::Env env, uint32_t qid)
		: id(qid)
		, deferred(Napi::Promise::Deferred::New(env))
		, sem(0) // semaphore is initially locked
	{
		// left empty intentionally
	}
};

#endif
