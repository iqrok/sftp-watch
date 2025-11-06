#ifndef _SFTP_HELPER_HPP
#define _SFTP_HELPER_HPP

#include "sftp_node.hpp"
#include <cstdint>

namespace SftpHelper {

void    shutdown();
int32_t connect(SftpWatch_t* ctx);
void    disconnect(SftpWatch_t* ctx);
int32_t auth(SftpWatch_t* ctx);
int32_t open_dir(SftpWatch_t* ctx, RemoteDir_t* dir);
int32_t close_dir(SftpWatch_t* ctx, RemoteDir_t* dir);
int32_t read_dir(RemoteDir_t& dir, DirItem_t* file);
int32_t copy_symlink_remote(SftpWatch_t* ctx, DirItem_t* file);
int32_t copy_file_remote(SftpWatch_t* ctx, DirItem_t* file);
int32_t remove_local(SftpWatch_t* ctx, std::string filename);
int32_t mkdir_local(SftpWatch_t* ctx, DirItem_t* file);
void    rmdir_local(SftpWatch_t* ctx, std::string dirname);
uint8_t get_filetype(DirItem_t* file);

}

#endif
