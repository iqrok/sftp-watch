#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>

#include <napi.h>

#include "sftp_helper.hpp"
#include "sftp_node.hpp"

typedef struct NewAndRemovedFiles_s {
	PairFileDet_t created;
	PairFileDet_t removed;
} NewAndRemovedFiles_t;

static uint32_t                         ids = 0;
static std::map<uint32_t, SftpWatch_t*> watchers;

static Napi::Value js_connect(const Napi::CallbackInfo& info);
static Napi::Value js_sync_dir(const Napi::CallbackInfo& info);

static void finalizer_sync_dir(
	Napi::Env env, void* finalizeData, SftpWatch_t* ctx)
{
	watchers.erase(ctx->id);

	ctx->thread.join();
	ctx->deferred.Resolve(Napi::Boolean::New(env, true));

	delete ctx;
}

static void sync_dir_tsfn_cb(
	Napi::Env env, Napi::Function js_cb, NewAndRemovedFiles_t* files)
{
	uint32_t    i       = 0;
	Napi::Array created = Napi::Array::New(env, files->created.size());

	for (const auto& pair : files->created) {
		DirItem_t    item = pair.second;
		Napi::Object obj  = Napi::Object::New(env);

		obj.Set("name", Napi::String::New(env, pair.first));
		obj.Set("type", Napi::String::New(env, std::string(1, item.type)));
		obj.Set("size", Napi::Number::New(env, item.attrs.filesize));
		obj.Set("time", Napi::Number::New(env, item.attrs.mtime));
		obj.Set("perm", Napi::Number::New(env, item.attrs.permissions));

		created.Set(i++, obj);
	}

	i                   = 0;
	Napi::Array removed = Napi::Array::New(env, files->removed.size());

	for (const auto& pair : files->removed) {
		DirItem_t    item = pair.second;
		Napi::Object obj  = Napi::Object::New(env);

		obj.Set("name", Napi::String::New(env, pair.first));
		obj.Set("type", Napi::String::New(env, std::string(1, item.type)));
		obj.Set("size", Napi::Number::New(env, item.attrs.filesize));
		obj.Set("time", Napi::Number::New(env, item.attrs.mtime));
		obj.Set("perm", Napi::Number::New(env, item.attrs.permissions));

		removed.Set(i++, obj);
	}

	Napi::Object collection = Napi::Object::New(env);
	collection.Set("created", created);
	collection.Set("removed", removed);

	js_cb.Call({ collection });
}

static void thread_sync_dir(SftpWatch_t* ctx)
{
	while (!ctx->is_stopped) {
		NewAndRemovedFiles_t list;
		PairFileDet_t        current;
		DirItem_t            item;

		// open remote dir first
		int32_t rc = SftpHelper::open_dir(ctx, ctx->remote_path.c_str());
		if (rc) {
			fprintf(stderr, "Failed to open path\n");
			break;
		}

		// read the opened directory
		while ((rc = SftpHelper::read_dir(ctx, &item)) != 0) {
			current[std::string(item.name)] = item;
		}

		// Check for new or modified files
		for (const auto& [key, val] : current) {
			DirItem_t now = val;
			DirItem_t old = ctx->last_files[key];

			if (now.name == "." || now.name == "..") continue;

			bool is_new = !ctx->last_files.contains(key)
				|| old.attrs.filesize != now.attrs.filesize
				|| old.attrs.mtime != now.attrs.mtime;

			if (is_new) {
				list.created[key] = now;

				// only write regular file
				if (now.type == IS_REG_FILE) {
					SftpHelper::sync_remote(ctx, &now);
				}

				// TODO: sync directory and its tree, until reached max depth
			}
		}

		// check for deleted files
		for (const auto& [key, val] : ctx->last_files) {
			if (!current.contains(key)) {
				SftpHelper::remove_local(ctx, val.name);
				list.removed[key] = val;
			}
		}

		// update last data
		ctx->last_files = current;

		if (ctx->tsfn.NonBlockingCall(&list, sync_dir_tsfn_cb) != napi_ok) {
			Napi::Error::Fatal("thread_si", "NonBlockingCall() failed");
		}

		std::this_thread::sleep_for(std::chrono::microseconds(ctx->delay_us));
	}

	// outside loop means no more operation to do. Cleanup the connection
	SftpHelper::cleanup(ctx);
}

static Napi::Value js_connect(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsObject()) {
		Napi::TypeError::New(env, "Expected object")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	Napi::Object arg = info[0].As<Napi::Object>();

	std::string host;
	uint16_t    port = 22U;
	std::string username;
	std::string pubkey;
	std::string privkey;
	std::string password;
	std::string remote_path;
	std::string local_path;

	if (arg.Has("host")) {
		if (!arg.Get("host").IsString() || arg.Get("host").IsEmpty()) {
			Napi::TypeError::New(env, "'host' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		host = arg.Get("host").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError::New(env, "'host' is undefined")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("username")) {
		if (!arg.Get("host").IsString() || arg.Get("host").IsEmpty()) {
			Napi::TypeError::New(env, "'username' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		username = arg.Get("username").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError::New(env, "'username' is undefined")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("remotePath")) {
		if (!arg.Get("remotePath").IsString()
			|| arg.Get("remotePath").IsEmpty()) {
			Napi::TypeError::New(env, "'remotePath' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		remote_path = arg.Get("remotePath").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError::New(env, "'remotePath' is undefined")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (arg.Has("localPath")) {
		if (!arg.Get("localPath").IsString()
			|| arg.Get("localPath").IsEmpty()) {
			Napi::TypeError::New(env, "'localPath' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		local_path = arg.Get("localPath").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("pubkey")) {
		if (!arg.Get("pubkey").IsString() || arg.Get("pubkey").IsEmpty()) {
			Napi::TypeError::New(env, "'pubkey' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		pubkey = arg.Get("pubkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("privkey")) {
		if (!arg.Get("privkey").IsString() || arg.Get("privkey").IsEmpty()) {
			Napi::TypeError::New(env, "'privkey' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		privkey = arg.Get("privkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("password")) {
		if (!arg.Get("password").IsString() || arg.Get("password").IsEmpty()) {
			Napi::TypeError::New(env, "'password' is empty")
				.ThrowAsJavaScriptException();
			return Napi::Boolean::New(env, false);
		}

		password = arg.Get("password").As<Napi::String>().Utf8Value();
	}

	if ((pubkey.empty() || privkey.empty()) && password.empty()) {
		Napi::TypeError::New(env, "invalid auth").ThrowAsJavaScriptException();
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

	ctx->host        = host;
	ctx->port        = port;
	ctx->username    = username;
	ctx->pubkey      = pubkey;
	ctx->privkey     = privkey;
	ctx->password    = password;
	ctx->remote_path = remote_path;
	ctx->local_path  = local_path;

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

	if (info.Length() < 3) {
		Napi::TypeError::New(
			env, "Invalid params. Should be <context_id, path>")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[0].IsNumber()) {
		Napi::TypeError::New(env, "Invalid context id. Should be a number")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[1].IsString()) {
		Napi::TypeError::New(env, "Invalid Remote Path. Should be a string")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[2].IsString()) {
		Napi::TypeError::New(env, "Invalid Local Path. Should be a string")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[3].IsFunction()) {
		Napi::TypeError::New(env, "Invalid Callback Function")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	const uint32_t    id          = info[0].As<Napi::Number>().Uint32Value();
	const std::string remote_path = info[1].As<Napi::String>().Utf8Value();
	const std::string local_path  = info[2].As<Napi::String>().Utf8Value();
	Napi::Function    js_cb       = info[3].As<Napi::Function>();

	SftpWatch_t* ctx = watchers.at(id);

	ctx->is_stopped = false;
	ctx->tsfn       = Napi::ThreadSafeFunction::New(env, // Environment
			  js_cb, // JS function from caller
			  std::string("watcher_") + std::to_string(id), // Resource name
			  0,                  // Max queue_si size (0 = unlimited).
			  1,                  // Initial thread count
			  ctx,                // Context,
			  finalizer_sync_dir, // Finalizer
			  (void*)nullptr      // Finalizer data
		  );

	ctx->thread = std::thread(thread_sync_dir, ctx);

	return Napi::Boolean::New(env, true);
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
