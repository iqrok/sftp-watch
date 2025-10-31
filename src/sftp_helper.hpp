#ifndef _SFTP_HELPER_HPP
#define _SFTP_HELPER_HPP

#include "sftp_node.hpp"
#include <cstdint>

namespace SftpHelper {

int32_t connect(SftpWatch_t* ctx);
int32_t auth(SftpWatch_t* ctx);

}

#endif
