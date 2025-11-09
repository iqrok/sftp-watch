#ifndef _SFTP_LOCAL_HPP
#define _SFTP_LOCAL_HPP

#include "sftp_node.hpp"

#include <cstdint>

namespace SftpLocal {

int32_t mkdir(SftpWatch_t* ctx, DirItem_t* file);

int32_t remove(SftpWatch_t* ctx, DirItem_t* file);
int32_t remove(SftpWatch_t* ctx, std::string& filename);

void rmdir(SftpWatch_t* ctx, DirItem_t* file);
void rmdir(SftpWatch_t* ctx, std::string& dirname);

}
#endif
