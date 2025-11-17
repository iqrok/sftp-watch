#ifndef _SFTP_LOCAL_HPP
#define _SFTP_LOCAL_HPP

#include "sftp_watch.hpp"

#include <cstdint>

namespace SftpLocal {

int32_t filestat(
	SftpWatch_t* ctx, std::string& path, LIBSSH2_SFTP_ATTRIBUTES* res);

int32_t open_dir(SftpWatch_t* ctx, Directory_t* dir);
int32_t close_dir(SftpWatch_t* ctx, Directory_t* dir);
int32_t read_dir(Directory_t& dir, DirItem_t* file);
int32_t mkdir(SftpWatch_t* ctx, DirItem_t* file);

int32_t remove(SftpWatch_t* ctx, DirItem_t* file);
int32_t remove(SftpWatch_t* ctx, std::string& filename);

void rmdir(SftpWatch_t* ctx, DirItem_t* file);
void rmdir(SftpWatch_t* ctx, std::string& dirname);

}
#endif
