#include <cstdint>
#include <map>
#include <thread>

#include <napi.h>

#include "sftp_helper.hpp"
#include "sftp_node.hpp"

static uint32_t                         ids = 0;
static std::map<uint32_t, SftpWatch_t*> watchers;

static Napi::Value js_connect(const Napi::CallbackInfo& info);
static Napi::Value js_sync_dir(const Napi::CallbackInfo& info);

static Napi::Value js_connect(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsObject()) {
		Napi::TypeError ::New(env, "Expected object")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	Napi::Object arg = info[0].As<Napi::Object>();

	std::string host;
	uint16_t    port = 22U;
	std::string username;
	std::string path;
	std::string pubkey;
	std::string privkey;
	std::string password;

	if (arg.Has("host")) {
		if (!arg.Get("host").IsString() || arg.Get("host").IsEmpty()) {
			Napi::TypeError ::New(env, "'host' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		host = arg.Get("host").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError ::New(env, "'host' is undefined")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("username")) {
		if (!arg.Get("host").IsString() || arg.Get("host").IsEmpty()) {
			Napi::TypeError ::New(env, "'username' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		username = arg.Get("username").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError ::New(env, "'username' is undefined")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	//~ if (arg.Has("path")) {
	//~ if (!arg.Get("path").IsString() || arg.Get("path").IsEmpty()) {
	//~ Napi::TypeError ::New(env, "'path' is empty")
	//~ .ThrowAsJavaScriptException();
	//~ return Napi::Boolean::New(env, false);
	//~ }

	//~ path = arg.Get("path").As<Napi::String>().Utf8Value();
	//~ } else {
	//~ Napi::TypeError ::New(env, "'path' is undefined")
	//~ .ThrowAsJavaScriptException();
	//~ return Napi::Boolean::New(env, false);
	//~ }

	if (arg.Has("pubkey")) {
		if (!arg.Get("pubkey").IsString() || arg.Get("pubkey").IsEmpty()) {
			Napi::TypeError ::New(env, "'pubkey' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		pubkey = arg.Get("pubkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("privkey")) {
		if (!arg.Get("privkey").IsString() || arg.Get("privkey").IsEmpty()) {
			Napi::TypeError ::New(env, "'privkey' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		privkey = arg.Get("privkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("password")) {
		if (!arg.Get("password").IsString() || arg.Get("password").IsEmpty()) {
			Napi::TypeError ::New(env, "'password' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		password = arg.Get("password").As<Napi::String>().Utf8Value();
	}

	if ((pubkey.empty() || privkey.empty()) && password.empty()) {
		Napi::TypeError ::New(env, "invalid auth").ThrowAsJavaScriptException();
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

	SftpWatch_t* ctx = new SftpWatch_t(env, ++ids);

	ctx->host     = host;
	ctx->port     = port;
	ctx->username = username;
	//~ ctx->path     = path;
	ctx->pubkey   = pubkey;
	ctx->privkey  = privkey;
	ctx->password = password;

	if (SftpHelper::connect(ctx)) {
		Napi::TypeError::New(env, "Can't connect to sftp server!")
			.ThrowAsJavaScriptException();

		delete ctx;

		return Napi::Boolean::New(env, false);
	}

	if (SftpHelper::auth(ctx)) {
		Napi::TypeError::New(env, "Authentication Failed!")
			.ThrowAsJavaScriptException();

		delete ctx;

		return Napi::Boolean::New(env, false);
	}

	watchers[ctx->id] = ctx;

	return Napi::Number::New(env, ctx->id);
}

static Napi::Value js_sync_dir(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 2) {
		Napi::TypeError ::New(
			env, "Invalid params. Should be <context_id, path>")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[0].IsNumber()) {
		Napi::TypeError ::New(env, "Invalid context id. Should be a number")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[1].IsString()) {
		Napi::TypeError ::New(env, "Invalid Remote Path. Should be a string")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[2].IsString()) {
		Napi::TypeError ::New(env, "Invalid Local Path. Should be a string")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	const uint32_t    id          = info[0].As<Napi::Number>().Uint32Value();
	const std::string remote_path = info[1].As<Napi::String>().Utf8Value();
	const std::string local_path  = info[2].As<Napi::String>().Utf8Value();

	SftpWatch_t* ctx = watchers.at(id);

	int32_t rc = SftpHelper::open_dir(ctx, remote_path.c_str());

	if (rc) {
		Napi::TypeError ::New(env, "Failed to open directory")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	SftpHelper::DirItem_t item;
	while ((rc = read_dir(ctx, &item))) {
		fprintf(stdout, "[flags %02x] %s size: %u  modified at %u\n",
			item.attrs.flags, item.name, item.attrs.filesize, item.attrs.mtime);
	}

	return Napi::Number::New(env, 1);
}

static Napi::Object init_napi(Napi::Env env, Napi::Object exports)
{
	exports.Set(Napi::String::New(env, "connect"),
		Napi::Function::New(env, js_connect));
	exports.Set(
		Napi::String::New(env, "sync"), Napi::Function::New(env, js_sync_dir));

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, init_napi);
