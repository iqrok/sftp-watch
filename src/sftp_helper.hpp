#ifndef _SFTP_HELPER_HPP
#define _SFTP_HELPER_HPP

#include "sftp_node.hpp"
#include <cstdint>

namespace SftpHelper {

constexpr uint16_t filename_max_len = 512;

typedef struct DirItem_s {
	char                    name[filename_max_len];
	LIBSSH2_SFTP_ATTRIBUTES attrs;
	/*
	 * LIBSSH2_SFTP_S_ISLNK  Test for a symbolic link
	 * LIBSSH2_SFTP_S_ISREG  Test for a regular file
	 * LIBSSH2_SFTP_S_ISDIR  Test for a directory
	 * LIBSSH2_SFTP_S_ISCHR  Test for a character special file
	 * LIBSSH2_SFTP_S_ISBLK  Test for a block special file
	 * LIBSSH2_SFTP_S_ISFIFO Test for a pipe or FIFO special file
	 * LIBSSH2_SFTP_S_ISSOCK Test for a socket
	 * */
} DirItem_t;

int32_t connect(SftpWatch_t* ctx);
int32_t auth(SftpWatch_t* ctx);
int32_t open_dir(SftpWatch_t* ctx, const char* remote_path);
int32_t read_dir(SftpWatch_t* ctx, DirItem_t* file);

}

#endif
