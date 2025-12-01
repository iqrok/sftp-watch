#ifndef _SNOD_SFTP_ERR_HPP
#define _SNOD_SFTP_ERR_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

namespace SftpErr {

const char* session_error(int32_t code);
const char* sftp_error(int32_t code);

}

#endif
