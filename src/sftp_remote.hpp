#ifndef _SFTP_REMOTE_HPP
#define _SFTP_REMOTE_HPP

#include "sftp_watch.hpp"
#include <cstdint>

namespace SftpRemote {

void    shutdown();
int32_t connect(SftpWatch_t* ctx);
void    disconnect(SftpWatch_t* ctx);
int32_t auth(SftpWatch_t* ctx);
int32_t open_dir(SftpWatch_t* ctx, Directory_t* dir);
int32_t close_dir(SftpWatch_t* ctx, Directory_t* dir);
int32_t mkdir(SftpWatch_t* ctx, DirItem_t* dir);
int32_t rmdir(SftpWatch_t* ctx, DirItem_t* dir);
int32_t read_dir(Directory_t& dir, DirItem_t* file);
int32_t down_symlink(SftpWatch_t* ctx, DirItem_t* file);
int32_t down_file(SftpWatch_t* ctx, DirItem_t* file);
int32_t up_file(SftpWatch_t* ctx, DirItem_t* file);
int32_t remove(SftpWatch_t* ctx, DirItem_t* file);
int32_t set_filestat(
	SftpWatch_t* ctx, std::string& path, LIBSSH2_SFTP_ATTRIBUTES* attrs);
int32_t get_filestat(
	SftpWatch_t* ctx, std::string& path, LIBSSH2_SFTP_ATTRIBUTES* attrs);

}

#endif
