/* Copyright (C) The libssh2 project and its contributors.
 *
 * Sample doing an SFTP directory listing.
 *
 * The sample code has default values for host name, user name, password and
 * path, but you can specify them on the command line like:
 *
 * $ ./sftpdir_nonblock 192.168.0.1 user password /tmp/secretdir
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <map>
#include <thread>
#include <cstdint>

#include <napi.h>

#include "libssh2_setup.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef __linux__
#	include <sys/socket.h>
#	include <unistd.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#endif

#include <cstdio>

#ifdef _MSC_VER
#define LIBSSH2_FILESIZE_MASK "I64u"
#else
#define LIBSSH2_FILESIZE_MASK "llu"
#endif

#define SFTP_TARGET_USERNAME "cadaver"
#define SFTP_TARGET_PORT	 65432

/*
//~ #define SFTP_OPEN_DIR(TP) do { \
		//~ sftp_handle = libssh2_sftp_opendir(sftp_session, TP); \
		//~ \
		//~ if(!sftp_handle && libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) { \
			//~ fprintf(stderr, "Unable to open dir with SFTP\n"); \
			//~ goto shutdown; \
		//~ } \
	//~ } while(!sftp_handle)

//~ static const char *pubkey = "/home/cadaver/.ssh/id_rsa.pub";
//~ static const char *privkey = "/home/cadaver/.ssh/id_rsa";
//~ static const char *username = SFTP_TARGET_USERNAME;
//~ static const char *password = "password";
//~ static const char *sftppath = "/tmp";
*/

typedef struct SftpWatch_s SftpWatch_t;

static bool	 is_inited  = false;

static int32_t  rc  = 0;
static uint32_t ids = 0;
static std::map<uint32_t, SftpWatch_t*> watchers;

struct SftpWatch_s {
	uint32_t	id;
	std::string host;
	uint16_t	port;
	std::string pubkey;
	std::string privkey;
	std::string username;
	std::string password;
	std::string path;

	libssh2_socket_t sock;
	LIBSSH2_SESSION* session = NULL;
	LIBSSH2_SFTP* sftp_session;
	LIBSSH2_SFTP_HANDLE* sftp_handle;
	struct sockaddr_in sin;
	const char* fingerprint;

	Napi::Promise::Deferred deferred;
	std::thread thread;
	Napi::ThreadSafeFunction tsfn;

	SftpWatch_s(Napi::Env env, uint32_t qid)
		: id(qid), deferred(Napi::Promise::Deferred::New(env)) {}
};

int32_t sftp_connect(SftpWatch_t* ctx);
int32_t sftp_auth(SftpWatch_t* ctx);

Napi::Value js_add_watcher(const Napi::CallbackInfo& info);

int32_t sftp_connect(SftpWatch_t* ctx)
{
	ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
	if(ctx->sock == LIBSSH2_INVALID_SOCKET) {
		fprintf(stderr, "failed to create socket.\n");
		return -1;
	}

	ctx->sin.sin_family = AF_INET;
	ctx->sin.sin_port = htons(ctx->port);
	ctx->sin.sin_addr.s_addr = inet_addr(ctx->host.c_str());
	rc = connect(
		ctx->sock, (struct sockaddr*)(&ctx->sin), sizeof(struct sockaddr_in));
	if(rc) {
		fprintf(stderr, "failed to connect. (%d) [%s:%u]\n", rc, ctx->host.c_str(), ctx->port);
		return -1;
	}

	/* Create a session instance */
	ctx->session = libssh2_session_init();
	if(!ctx->session) {
		fprintf(stderr, "Could not initialize SSH session.\n");
		return -1;
	}

	/* Since we have set non-blocking, tell libssh2 we are non-blocking */
	libssh2_session_set_blocking(ctx->session, 0);

	while((rc = libssh2_session_handshake(ctx->session, ctx->sock))
		== LIBSSH2_ERROR_EAGAIN);

	if (rc) {
		fprintf(stderr, "Failure establishing SSH session: %d\n", rc);
		return -1;
	}

	ctx->fingerprint = libssh2_hostkey_hash(ctx->session, LIBSSH2_HOSTKEY_HASH_SHA1);
	fprintf(stderr, "Fingerprint: ");
	for(uint8_t i = 0; i < 20; i++) {
		fprintf(stderr, "%02X ", (unsigned char)ctx->fingerprint[i]);
	}
	fprintf(stderr, "\n");

	return 0;
}

int32_t sftp_auth(SftpWatch_t* ctx)
{
	// pubkey auth if both pubkey and privkey are provided
	if (!ctx->pubkey.empty() && !ctx->privkey.empty()) {
		while((rc = libssh2_userauth_publickey_fromfile(ctx->session,
			ctx->username.c_str(), ctx->pubkey.c_str(), ctx->privkey.c_str(),
			ctx->password.c_str())) == LIBSSH2_ERROR_EAGAIN);

		if (rc) {
			fprintf(stderr, "Authentication by public key failed.\n");
			return -1;
		}
	}

	// or password if privkey is not provided
	else if (!ctx->password.empty()) {
		while((rc = libssh2_userauth_password(ctx->session, ctx->username.c_str(),
			ctx->password.c_str())) == LIBSSH2_ERROR_EAGAIN);

		if (rc) {
			fprintf(stderr, "Authentication by password failed.\n");
			return -1;
		}
	}

	// welp, no password no pubkey, wtf do you want!? is the admin stupid.
	else {
		fprintf(stderr, "No Valid Authentication is provided.\n");
		return -2;
	}

	//~ fprintf(stderr, "libssh2_sftp_init().\n");
	do {
		ctx->sftp_session = libssh2_sftp_init(ctx->session);

		if(!ctx->sftp_session &&
		   libssh2_session_last_errno(ctx->session) != LIBSSH2_ERROR_EAGAIN) {
			fprintf(stderr, "Unable to init SFTP session\n");
			return -3;
		}
	} while(!ctx->sftp_session);

	return 0;
}

Napi::Value js_connect(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsObject()) {
		Napi::TypeError
			::New(env, "Expected object").ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	Napi::Object arg = info[0].As<Napi::Object>();

	std::string host;
	uint16_t	port = 22U;
	std::string username;
	std::string path;
	std::string pubkey;
	std::string privkey;
	std::string password;

	if (arg.Has("host")) {
		if (!arg.Get("host").IsString() || arg.Get("host").IsEmpty()) {
			Napi::TypeError
				::New(env, "'host' is empty").ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		host = arg.Get("host").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError
			::New(env, "'host' is undefined").ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("username")) {
		if (!arg.Get("host").IsString() || arg.Get("host").IsEmpty()) {
			Napi::TypeError
				::New(env, "'username' is empty").ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		username = arg.Get("username").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError
			::New(env, "'username' is undefined").ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("path")) {
		if (!arg.Get("path").IsString() || arg.Get("path").IsEmpty()) {
			Napi::TypeError
				::New(env, "'path' is empty").ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		path = arg.Get("path").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError
			::New(env, "'path' is undefined").ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("pubkey")) {
		if (!arg.Get("pubkey").IsString() || arg.Get("pubkey").IsEmpty()) {
			Napi::TypeError
				::New(env, "'pubkey' is empty").ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		pubkey = arg.Get("pubkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("privkey")) {
		if (!arg.Get("privkey").IsString() || arg.Get("privkey").IsEmpty()) {
			Napi::TypeError
				::New(env, "'privkey' is empty").ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		privkey = arg.Get("privkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("password")) {
		if (!arg.Get("password").IsString() || arg.Get("password").IsEmpty()) {
			Napi::TypeError
				::New(env, "'password' is empty").ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		password = arg.Get("password").As<Napi::String>().Utf8Value();
	}

	if ((pubkey.empty() || privkey.empty()) && password.empty()) {
		Napi::TypeError
			::New(env, "invalid auth").ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("port")) {
		uint32_t tmp = arg.Get("port").As<Napi::Number>().Uint32Value();

		if (tmp > 0xFFFF || tmp == 0) {
			Napi::TypeError::New(env, "'port' number max is 65535")
				.ThrowAsJavaScriptException();

			return Napi::Boolean::New(env, false);
		}

		port = static_cast<uint16_t>(tmp);
	} else {
		fprintf(stderr, "Port is empty. Will use port %u\n", port);
	}

	if ((rc = libssh2_init(0)) != 0) {
		fprintf(stderr, "libssh2 initialization failed (%d)\n", rc);

		Napi::TypeError::New(env, "libssh2 initialization failed!")
			.ThrowAsJavaScriptException();

		return Napi::Boolean::New(env, false);
	}

	is_inited = true;

	SftpWatch_t* ctx = new SftpWatch_t(env, ++ids);

	ctx->host	  = host;
	ctx->port 	  = port;
	ctx->username = username;
	ctx->path	  = path;
	ctx->pubkey   = pubkey;
	ctx->privkey  = privkey;
	ctx->password = password;

	if (sftp_connect(ctx)) {
		Napi::TypeError::New(env, "Can't connect to sftp server!")
			.ThrowAsJavaScriptException();

		delete ctx;

		return Napi::Boolean::New(env, false);
	}

	if (sftp_auth(ctx)) {
		Napi::TypeError::New(env, "Authentication Failed!")
			.ThrowAsJavaScriptException();

		delete ctx;

		return Napi::Boolean::New(env, false);
	}

	watchers[ctx->id] = ctx;

	return Napi::Number::New(env, ctx->id);
}

Napi::Object init_napi(Napi::Env env, Napi::Object exports)
{
	exports.Set(Napi::String::New(env, "connect"),
		Napi::Function::New(env, js_connect));

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, init_napi);

//~ int main(int argc, char *argv[])
//~ {
	//~ uint32_t hostaddr;
	//~ libssh2_socket_t sock;
	//~ int i, auth_pw = 0;
	//~ struct sockaddr_in sin;
	//~ const char *fingerprint;
	//~ int rc;
	//~ LIBSSH2_SESSION *session = NULL;
	//~ LIBSSH2_SFTP *sftp_session;
	//~ LIBSSH2_SFTP_HANDLE *sftp_handle;

//~ #ifdef _WIN32
	//~ WSADATA wsadata;

	//~ rc = WSAStartup(MAKEWORD(2, 0), &wsadata);
	//~ if(rc) {
		//~ fprintf(stderr, "WSAStartup failed with error: %d\n", rc);
		//~ return 1;
	//~ }
//~ #endif

	//~ if(argc > 1) {
		//~ hostaddr = inet_addr(argv[1]);
	//~ }
	//~ else {
		//~ hostaddr = htonl(0x7F000001);
	//~ }
	//~ if(argc > 2) {
		//~ username = argv[2];
	//~ }
	//~ if(argc > 3) {
		//~ password = argv[3];
	//~ }
	//~ if(argc > 4) {
		//~ sftppath = argv[4];
	//~ }

	//~ rc = libssh2_init(0);
	//~ if(rc) {
		//~ fprintf(stderr, "libssh2 initialization failed (%d)\n", rc);
		//~ return 1;
	//~ }

	//~ /*
	 //~ * The application code is responsible for creating the socket
	 //~ * and establishing the connection
	 //~ */
	//~ sock = socket(AF_INET, SOCK_STREAM, 0);
	//~ if(sock == LIBSSH2_INVALID_SOCKET) {
		//~ fprintf(stderr, "failed to create socket.\n");
		//~ goto shutdown;
	//~ }

	//~ sin.sin_family = AF_INET;
	//~ sin.sin_port = htons(SFTP_TARGET_PORT);
	//~ sin.sin_addr.s_addr = hostaddr;
	//~ if(connect(sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in))) {
		//~ fprintf(stderr, "failed to connect.\n");
		//~ goto shutdown;
	//~ }

	//~ /* Create a session instance */
	//~ session = libssh2_session_init();
	//~ if(!session) {
		//~ fprintf(stderr, "Could not initialize SSH session.\n");
		//~ goto shutdown;
	//~ }

	//~ /* Since we have set non-blocking, tell libssh2 we are non-blocking */
	//~ libssh2_session_set_blocking(session, 0);

	//~ /* ... start it up. This will trade welcome banners, exchange keys,
	 //~ * and setup crypto, compression, and MAC layers
	 //~ */
	//~ while((rc = libssh2_session_handshake(session, sock)) ==
		  //~ LIBSSH2_ERROR_EAGAIN);
	//~ if(rc) {
		//~ fprintf(stderr, "Failure establishing SSH session: %d\n", rc);
		//~ goto shutdown;
	//~ }

	//~ /* At this point we have not yet authenticated.  The first thing to do
	 //~ * is check the hostkey's fingerprint against our known hosts Your app
	 //~ * may have it hard coded, may go to a file, may present it to the
	 //~ * user, that's your call
	 //~ */
	//~ fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);
	//~ fprintf(stderr, "Fingerprint: ");
	//~ for(i = 0; i < 20; i++) {
		//~ fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
	//~ }
	//~ fprintf(stderr, "\n");

	//~ if(auth_pw) {
		//~ /* We could authenticate via password */
		//~ while((rc = libssh2_userauth_password(session, username, password)) ==
			  //~ LIBSSH2_ERROR_EAGAIN);
		//~ if(rc) {
			//~ fprintf(stderr, "Authentication by password failed.\n");
			//~ goto shutdown;
		//~ }
	//~ }
	//~ else {
		//~ /* Or by public key */
		//~ while((rc = libssh2_userauth_publickey_fromfile(session, username,
														//~ pubkey, privkey,
														//~ password)) ==
			  //~ LIBSSH2_ERROR_EAGAIN);
		//~ if(rc) {
			//~ fprintf(stderr, "Authentication by public key failed.\n");
			//~ goto shutdown;
		//~ }
	//~ }

	//~ fprintf(stderr, "libssh2_sftp_init().\n");
	//~ do {
		//~ sftp_session = libssh2_sftp_init(session);

		//~ if(!sftp_session &&
		   //~ libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
			//~ fprintf(stderr, "Unable to init SFTP session\n");
			//~ goto shutdown;
		//~ }
	//~ } while(!sftp_session);

	//~ fprintf(stderr, "libssh2_sftp_opendir().\n");
	//~ /* Request a dir listing via SFTP */
	//~ SFTP_OPEN_DIR(sftppath);

	//~ fprintf(stderr, "libssh2_sftp_opendir() is done, now receive listing.\n");
	//~ do {
		//~ char mem[512];
		//~ LIBSSH2_SFTP_ATTRIBUTES attrs;

		//~ /* loop until we fail */
		//~ while((rc = libssh2_sftp_readdir(sftp_handle, mem, sizeof(mem),
										 //~ &attrs)) == LIBSSH2_ERROR_EAGAIN);
		//~ if(rc > 0) {
			//~ /* rc is the length of the file name in the mem
			   //~ buffer */

			//~ if(attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
				//~ /* this should check what permissions it
				   //~ is and print the output accordingly */
				//~ printf("--fix----- ");
			//~ }
			//~ else {
				//~ printf("---------- ");
			//~ }

			//~ if(attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
				//~ printf("%4d %4d ", (int) attrs.uid, (int) attrs.gid);
			//~ }
			//~ else {
				//~ printf("   -	- ");
			//~ }

			//~ if(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
				//~ printf("%8" LIBSSH2_FILESIZE_MASK " ", attrs.filesize);
			//~ }

			//~ printf("%s\n", mem);
		//~ }
		//~ else if(rc == LIBSSH2_ERROR_EAGAIN) {
			//~ /* blocking */
			//~ fprintf(stderr, "Blocking\n");
		//~ }
		//~ else {
			//~ usleep(1000000);
			//~ SFTP_OPEN_DIR(sftppath);
			// break;
		//~ }

	//~ } while(1);

	//~ libssh2_sftp_closedir(sftp_handle);
	//~ libssh2_sftp_shutdown(sftp_session);

//~ shutdown:

	//~ if(session) {
		//~ libssh2_session_disconnect(session, "Normal Shutdown");
		//~ libssh2_session_free(session);
	//~ }

	//~ if(sock != LIBSSH2_INVALID_SOCKET) {
		//~ shutdown(sock, 2);
		//~ LIBSSH2_SOCKET_CLOSE(sock);
	//~ }

	//~ fprintf(stderr, "all done\n");

	//~ libssh2_exit();

//~ #ifdef _WIN32
	//~ WSACleanup();
//~ #endif

	//~ return 0;
//~ }
