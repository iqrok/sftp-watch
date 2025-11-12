
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
#	include <dirent.h>

#	define SNOD_RESET_ERRNO() errno = 0;
#elif defined(_WIN32)
#	include <windows.h>   // general Windows API, timeval replacement
#	include <io.h>        // _close, _read, _write (instead of unistd.h)
#	include <sys/types.h> // basic types
#	include <sys/stat.h>  // file status
#	include <sys/utime.h> // _utime(), _utimbuf

#	define write(f, b, c) write((f), (b), (unsigned int)(c))

#	define stat               _stat
#	define lstat              _stat // no equivalent for windows, use _stat
#	define S_ISDIR(mode)      ((mode) & _S_IFDIR)
#	define SNOD_RESET_ERRNO() (void)0;
#	define strerror(...)      "Error"
#elif
#	error "UNKNOWN ENVIRONMENT"
#endif

namespace {

static void conv_stat_attrs(LIBSSH2_SFTP_ATTRIBUTES* attrs, struct stat* st)
{
	attrs->flags = 0;

	attrs->filesize = (libssh2_uint64_t)st->st_size;
	attrs->flags |= (libssh2_uint64_t)st->st_size;

	attrs->uid = (unsigned long)st->st_uid;
	attrs->gid = (unsigned long)st->st_gid;
	attrs->flags |= LIBSSH2_SFTP_ATTR_UIDGID;

	attrs->permissions = (unsigned long)st->st_mode;
	attrs->flags |= LIBSSH2_SFTP_ATTR_PERMISSIONS;

	attrs->atime = (libssh2_uint64_t)st->st_atime;
	attrs->mtime = (libssh2_uint64_t)st->st_mtime;
	attrs->flags |= LIBSSH2_SFTP_ATTR_ACMODTIME;
}

}

uint8_t SftpLocal::get_filetype(DirItem_t* file)
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

int32_t SftpLocal::open_dir(SftpWatch_t* ctx, Directory_t* dir)
{
	if (dir->is_opened) SftpLocal::close_dir(ctx, dir);

#if defined(_POSIX_VERSION)

	errno = 0;
	if ((dir->loc_handle = opendir(dir->path.c_str())) == NULL) {
		LOG_ERR("Unable to open local dir '%s' '%s' with SFTP [%d] %s\n",
			dir->path.c_str(), dir->rela.c_str(), errno, strerror(errno));
		return errno;
	}

	dir->is_opened = true;

	return 0;

#elif defined(_WIN32)
	/*
	 * in Windows, opening dir is more like reading the first directory item.
	 * Just return success, and do it in read_dir.
	 * */
	return 0;
#else
#	error "UNKNOWN ENVIRONMENT"
#endif
}

int32_t SftpLocal::close_dir(SftpWatch_t* ctx, Directory_t* dir)
{
	(void)ctx;

	if (!dir->is_opened) return 0;

#if defined(_POSIX_VERSION)

	SNOD_RESET_ERRNO();
	if (closedir(dir->loc_handle)) {
		LOG_ERR("Unable to close local dir '%s' '%s' with SFTP [%d] %s\n",
			dir->path.c_str(), dir->rela.c_str(), errno, strerror(errno));
		return errno;
	}

#elif defined(_WIN32)

	if (!FindClose(dir->loc_handle)) {
		int32_t err = GetLastError();
		LOG_ERR("Unable to close local dir '%s' '%s' with SFTP [%d]\n",
			dir->path.c_str(), dir->rela.c_str(), err);
		return err;
	}

#else
#	error "UNKNOWN ENVIRONMENT"
#endif

	dir->is_opened = false;

	return 0;
}

int32_t SftpLocal::read_dir(Directory_t& dir, DirItem_t* file)
{
#if defined(_POSIX_VERSION)

	SNOD_RESET_ERRNO();
	struct dirent* dp;

	// readdir error or finished
	if ((dp = readdir(dir.loc_handle)) == NULL) {
		if (errno) {
			LOG_ERR("Unable to read local dir '%s' '%s' with SFTP [%d] %s\n",
				dir.path.c_str(), dir.rela.c_str(), errno, strerror(errno));
		}

		return 0;
	}

	// there's a record
	std::string name(dp->d_name);

#elif defined(_WIN32)

	WIN32_FIND_DATAA data;
	std::string      search_path = dir.path + "\\*";

	/*
	 * 1. Read the first file if is_opened is false
	 * 2. Read subsequent files
	 * */

	if (!dir.is_opened) {
		/*
		 * FIXME: when file deleted from remote, since no open_dir is done, the
		 *        inaccessible directory remains exist.
		 * */
		dir.loc_handle = FindFirstFileA(search_path.c_str(), &data);

		if (dir.loc_handle == INVALID_HANDLE_VALUE) {
			int32_t err = GetLastError();
			LOG_ERR("Unable to read first local '%s' '%s' [%d]\n",
				dir.path.c_str(), dir.rela.c_str(), err);
			return 0;
		}

		dir.is_opened = true;
	} else {
		if (!FindNextFileA(dir.loc_handle, &data)) {
			int32_t err = GetLastError();
			if (err != ERROR_NO_MORE_FILES) {
				LOG_ERR("Unable to read next local '%s' '%s' [%d]\n",
					dir.path.c_str(), dir.rela.c_str(), err);
			}
			return 0;
		}
	}

	std::string name(data.cFileName);

#else
#	error "UNKNOWN ENVIRONMENT"
#endif

	// Common Codes for POSIX and Windows
	struct stat st;
	std::string abs_path = dir.path + SNOD_SEP + name;

	if (name == "." || name == "..") {
		file->name           = "";
		file->type           = IS_INVALID;
		file->attrs.filesize = 0;
		file->attrs.mtime    = 0;
		return 1;
	}

	SNOD_RESET_ERRNO();
	if (lstat(abs_path.c_str(), &st)) {
		LOG_ERR("FAILED lstat local file '%s' '%s' [%d] %s\n", abs_path.c_str(),
			name.c_str(), errno, strerror(errno));
		return errno;
	}

	file->attrs.flags = 0;

	file->attrs.filesize = (libssh2_uint64_t)st.st_size;
	file->attrs.flags |= (libssh2_uint64_t)st.st_size;

	file->attrs.uid = (unsigned long)st.st_uid;
	file->attrs.gid = (unsigned long)st.st_gid;
	file->attrs.flags |= LIBSSH2_SFTP_ATTR_UIDGID;

	file->attrs.permissions = (unsigned long)st.st_mode;
	file->attrs.flags |= LIBSSH2_SFTP_ATTR_PERMISSIONS;

	file->attrs.atime = (libssh2_uint64_t)st.st_atime;
	file->attrs.mtime = (libssh2_uint64_t)st.st_mtime;
	file->attrs.flags |= LIBSSH2_SFTP_ATTR_ACMODTIME;

	file->type = SftpLocal::get_filetype(file);
	file->name = dir.rela.empty() ? name : dir.rela + SNOD_SEP + name;

	return 1;
}

int32_t SftpLocal::remove(SftpWatch_t* ctx, std::string& filename)
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
void SftpLocal::rmdir(SftpWatch_t* ctx, std::string& dirname)
{
	std::filesystem::path dirpath(ctx->local_path + SNOD_SEP + dirname);

	if (!std::filesystem::is_directory(dirpath)) return;

	std::filesystem::remove_all(dirpath);
}

void SftpLocal::rmdir(SftpWatch_t* ctx, DirItem_t* dir)
{
	return SftpLocal::rmdir(ctx, dir->name);
}

int32_t SftpLocal::filestat(
	SftpWatch_t* ctx, std::string& path, LIBSSH2_SFTP_ATTRIBUTES* res)
{
	(void)ctx;
	struct stat st;

	SNOD_RESET_ERRNO();
	if (lstat(path.c_str(), &st)) {
		LOG_ERR("FAILED lstat local file '%s' [%d] %s\n", path.c_str(), errno,
			strerror(errno));
		return errno;
	}

	conv_stat_attrs(res, &st);

	return 0;
}
