#ifndef _SFTP_LOCAL_HPP
#define _SFTP_LOCAL_HPP

#include "sftp_node.hpp"

#include <cstdint>

namespace SftpLocal {

uint8_t get_filetype(DirItem_t* file);
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
