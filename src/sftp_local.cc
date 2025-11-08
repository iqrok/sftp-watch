
#include "sftp_local.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <filesystem> // for removing directory

#if defined(_POSIX_VERSION)
#	include <sys/stat.h>
#	include <sys/time.h>
#	include <unistd.h>
#	include <utime.h>
#elif defined(_WIN32)
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

int32_t SftpLocal::remove(SftpWatch_t* ctx, std::string filename)
{
	std::string local_file = ctx->local_path + SNOD_SEP + filename;

	if (::remove(local_file.c_str())) {
		LOG_ERR(
			"Err %d: %s '%s'\n", errno, strerror(errno), local_file.c_str());
		return -1;
	}

	return 0;
}

int32_t SftpLocal::remove(SftpWatch_t* ctx, DirItem_t* file)
{
	return SftpLocal::remove(ctx, file->name);
}

int32_t SftpLocal::mkdir(SftpWatch_t* ctx, DirItem_t* file)
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
	rc = ::mkdir(local_dir.c_str(), SNOD_FILE_PERM(file->attrs));
	if (rc) {
		LOG_ERR("Failed create directory: %d\n", errno);
		return rc;
	}

	// set modified & access time time to match remote
	if (utime(local_dir.c_str(), &times)) {
		LOG_ERR("Failed to set mtime [%d]\n", errno);
	}
#endif

#ifdef _WIN32
	// NOTE: If the CreateDirectoryA succeeds, the return value is nonzero.
	rc = CreateDirectoryA(local_dir.c_str(), NULL);
	if (!rc) LOG_ERR("Failed create directory: %d\n", GetLastError());
#endif

	return rc;
}

/*
 * NOTE: using C++ filesystem to remove directory and its contents
 *       ref https://stackoverflow.com/a/50051546/3258981
 * */
void SftpLocal::rmdir(SftpWatch_t* ctx, std::string dirname)
{
	std::filesystem::path dirpath(ctx->local_path + SNOD_SEP + dirname);

	if (!std::filesystem::is_directory(dirpath)) return;

	std::filesystem::remove_all(dirpath);
}

void SftpLocal::rmdir(SftpWatch_t* ctx, DirItem_t* dir)
{
	return SftpLocal::rmdir(ctx, dir->name);
}
