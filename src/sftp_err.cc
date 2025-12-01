#include "sftp_err.hpp"

namespace {

static const std::unordered_map<int32_t, const char*> sftp_err_msg = {
	{ 0UL,  "SFTP_OK"					 },
    { 1UL,  "SFTP_EOF"                    },
    { 2UL,  "SFTP_NO_SUCH_FILE"           },
	{ 3UL,  "SFTP_PERMISSION_DENIED"      },
    { 4UL,  "SFTP_FAILURE"                },
	{ 5UL,  "SFTP_BAD_MESSAGE"            },
    { 6UL,  "SFTP_NO_CONNECTION"          },
	{ 7UL,  "SFTP_CONNECTION_LOST"        },
    { 8UL,  "SFTP_OP_UNSUPPORTED"         },
	{ 9UL,  "SFTP_INVALID_HANDLE"         },
    { 10UL, "SFTP_NO_SUCH_PATH"           },
	{ 11UL, "SFTP_FILE_ALREADY_EXISTS"    },
    { 12UL, "SFTP_WRITE_PROTECT"          },
	{ 13UL, "SFTP_NO_MEDIA"               },
    { 14UL, "SFTP_NO_SPACE_ON_FILESYSTEM" },
	{ 15UL, "SFTP_QUOTA_EXCEEDED"         },
    { 16UL, "SFTP_UNKNOWN_PRINCIPAL"      },
	{ 17UL, "SFTP_LOCK_CONFLICT"          },
    { 18UL, "SFTP_DIR_NOT_EMPTY"          },
	{ 19UL, "SFTP_NOT_A_DIRECTORY"        },
    { 20UL, "SFTP_INVALID_FILENAME"       },
	{ 21UL, "SFTP_LINK_LOOP"              }
};

static const std::unordered_map<int32_t, const char*> session_err_msg = {
	{ 0,   "ERR_NONE"					},
    { -1,  "ERR_SOCKET_NONE"             },
    { -2,  "ERR_BANNER_RECV"             },
	{ -3,  "ERR_BANNER_SEND"             },
    { -4,  "ERR_INVALID_MAC"             },
	{ -5,  "ERR_KEX_FAILURE"             },
    { -6,  "ERR_ALLOC"                   },
    { -7,  "ERR_SOCKET_SEND"             },
	{ -8,  "ERR_KEY_EXCHANGE_FAILURE"    },
    { -9,  "ERR_TIMEOUT"                 },
	{ -10, "ERR_HOSTKEY_INIT"            },
    { -11, "ERR_HOSTKEY_SIGN"            },
	{ -12, "ERR_DECRYPT"                 },
    { -13, "ERR_SOCKET_DISCONNECT"       },
	{ -14, "ERR_PROTO"                   },
    { -15, "ERR_PASSWORD_EXPIRED"        },
    { -16, "ERR_FILE"                    },
	{ -17, "ERR_METHOD_NONE"             },
    { -18, "ERR_AUTHENTICATION_FAILED"   },
	{ -19, "ERR_PUBLICKEY_UNVERIFIED"    },
    { -20, "ERR_CHANNEL_OUTOFORDER"      },
	{ -21, "ERR_CHANNEL_FAILURE"         },
    { -22, "ERR_CHANNEL_REQUEST_DENIED"  },
	{ -23, "ERR_CHANNEL_UNKNOWN"         },
    { -24, "ERR_CHANNEL_WINDOW_EXCEEDED" },
	{ -25, "ERR_CHANNEL_PACKET_EXCEEDED" },
    { -26, "ERR_CHANNEL_CLOSED"          },
	{ -27, "ERR_CHANNEL_EOF_SENT"        },
    { -28, "ERR_SCP_PROTOCOL"            },
	{ -29, "ERR_ZLIB"                    },
    { -30, "ERR_SOCKET_TIMEOUT"          },
	{ -31, "ERR_SFTP_PROTOCOL"           },
    { -32, "ERR_REQUEST_DENIED"          },
	{ -33, "ERR_METHOD_NOT_SUPPORTED"    },
    { -34, "ERR_INVAL"                   },
	{ -35, "ERR_INVALID_POLL_TYPE"       },
    { -36, "ERR_PUBLICKEY_PROTOCOL"      },
	{ -37, "ERR_EAGAIN"                  },
    { -38, "ERR_BUFFER_TOO_SMALL"        },
	{ -39, "ERR_BAD_USE"                 },
    { -40, "ERR_COMPRESS"                },
	{ -41, "ERR_OUT_OF_BOUNDARY"         },
    { -42, "ERR_AGENT_PROTOCOL"          },
	{ -43, "ERR_SOCKET_RECV"             },
    { -44, "ERR_ENCRYPT"                 },
	{ -45, "ERR_BAD_SOCKET"              },
    { -46, "ERR_KNOWN_HOSTS"             },
	{ -47, "ERR_CHANNEL_WINDOW_FULL"     },
    { -48, "ERR_KEYFILE_AUTH_FAILED"     },
	{ -49, "ERR_RANDGEN"                 },
    { -50, "ERR_MISSING_USERAUTH_BANNER" },
	{ -51, "ERR_ALGO_UNSUPPORTED"        },
    { -52, "ERR_MAC_FAILURE"             },
	{ -53, "ERR_HASH_INIT"               },
    { -54, "ERR_HASH_CALC"               }
};

}

const char* SftpErr::sftp_error(int32_t code)
{
	auto it = sftp_err_msg.find(code);
	return it == sftp_err_msg.end() ? nullptr : it->second;
}

const char* SftpErr::session_error(int32_t code)
{
	auto it = session_err_msg.find(code);
	return it == session_err_msg.end() ? nullptr : it->second;
}
