#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>

#include <napi.h>

#include "sftp_helper.hpp"
#include "sftp_node.hpp"

#define DELAY_US(us)                                                           \
	std::this_thread::sleep_for(std::chrono::microseconds((us)))
#define DELAY_MS(ms)                                                           \
	std::this_thread::sleep_for(std::chrono::milliseconds((ms)))

#define EVT_NAME_DEL "del"
#define EVT_NAME_NEW "new"
#define EVT_NAME_MOD "mod"

static uint32_t ids = 0;

static std::map<uint32_t, SftpWatch_t*> watchers;

static Napi::Value js_connect(const Napi::CallbackInfo& info);
static Napi::Value js_sync_dir(const Napi::CallbackInfo& info);

static int32_t connect_or_reconnect(SftpWatch_t* ctx)
{
	if (ctx->is_connected) SftpHelper::disconnect(ctx);

	if (SftpHelper::connect(ctx)) return -1;
	if (SftpHelper::auth(ctx)) return -2;

	return 0;
}

static void finalizer_sync_dir(
	Napi::Env env, void* finalizeData, SftpWatch_t* ctx)
{
	(void)finalizeData; // unused. btw it's a nullptr
	watchers.erase(ctx->id);

	ctx->thread.join();
	ctx->deferred.Resolve(Napi::Boolean::New(env, true));

	delete ctx;

	if (watchers.empty()) SftpHelper::shutdown();
}

static void sync_dir_tsfn_cb(
	Napi::Env env, Napi::Function js_cb, SftpWatch_t* ctx)
{
	Napi::Object obj = Napi::Object::New(env);

	obj.Set("evt", Napi::String::New(env, ctx->ev_file->name));
	obj.Set("name", Napi::String::New(env, ctx->ev_file->file->name));
	obj.Set("type",
		Napi::String::New(env, std::string(1, ctx->ev_file->file->type)));
	obj.Set("size", Napi::Number::New(env, ctx->ev_file->file->attrs.filesize));
	obj.Set(
		"time", Napi::Number::New(env, ctx->ev_file->file->attrs.mtime * 1e3));
	obj.Set("perm",
		Napi::Number::New(env, SNOD_FILE_PERM(ctx->ev_file->file->attrs)));

	// don't forget to delete the data, since we used dynamic allocation
	delete ctx->ev_file;

	js_cb.Call({ obj });

	// release the lock
	ctx->sem.release();
}

static void thread_sync_dir(SftpWatch_t* ctx)
{
	while (!ctx->is_stopped) {
		PairFileDet_t current;
		DirItem_t     item;

		RemoteDir_t dir;
		dir.path = ctx->remote_path;

		// open remote dir first
		int32_t rc = SftpHelper::open_dir(ctx, &dir);
		if (rc) {
			if (++ctx->err_count >= ctx->max_err_count) {
				connect_or_reconnect(ctx);
				ctx->err_count = 0;
				continue;
			}

			DELAY_MS(ctx->delay_ms);
			continue;
		}

		ctx->err_count = 0;

		// read the opened directory
		while ((rc = SftpHelper::read_dir(&dir, &item)) != 0) {
			current[std::string(item.name)] = item;
		}

		// Check for new or modified files
		for (const auto& [key, val] : current) {
			if (val.name == "." || val.name == "..") continue;

			DirItem_t now    = current[key];
			bool      is_mod = false;
			bool      is_new = !ctx->last_files.contains(key);

			if (!is_new) {
				is_mod = (ctx->last_files[key].attrs.filesize
							 != now.attrs.filesize)
					|| (ctx->last_files[key].attrs.mtime != now.attrs.mtime);
			}

			if (!(is_new || is_mod)) continue;

			// only write regular file
			switch (now.type) {

			case IS_REG_FILE: {
				// TODO: multiple files at once with single connection
				SftpHelper::sync_file_remote(ctx, &now);
			} break;

			// TODO: sync directory and its tree, until reached max depth
			case IS_DIR: {
				SftpHelper::mkdir_local(ctx, &now);
			} break;

			default: {
				// nothing to do for now
			} break;
			}

			ctx->ev_file       = new EvtFile_t;
			ctx->ev_file->name = is_new ? EVT_NAME_NEW : EVT_NAME_MOD;
			ctx->ev_file->file = &now;

			// BlockingCall() should never fail, since max queue size is 0
			if (ctx->tsfn.BlockingCall(ctx, sync_dir_tsfn_cb) != napi_ok) {
				Napi::Error::Fatal("new file err", "BlockingCall() failed");
			}

			// wait until the BlockingCall is finished
			ctx->sem.acquire();
		}

		for (const auto& [key, val] : ctx->last_files) {
			if (current.contains(key)) continue;

			DirItem_t old = val;

			switch (old.type) {

			case IS_DIR: {
				// TODO: check if dir is removed or renamed
			} break;

			/*
			 * NOTE: any file type should be safe enough to be remove directly,
			 *       right?
			 * */
			default: {
				SftpHelper::remove_local(ctx, old.name);
			} break;
			}

			ctx->ev_file       = new EvtFile_t;
			ctx->ev_file->name = EVT_NAME_DEL;
			ctx->ev_file->file = &old;

			if (ctx->tsfn.BlockingCall(ctx, sync_dir_tsfn_cb) != napi_ok) {
				Napi::Error::Fatal("del file err", "BlockingCall() failed");
			}

			// wait until the BlockingCall is finished, then lock
			ctx->sem.acquire();
		}

		// update last data
		ctx->last_files = current;

		// close opened dir
		SftpHelper::close_dir(ctx, &dir);

		DELAY_MS(ctx->delay_ms);
	}

	// outside loop means no more operation to do. Cleanup the connection
	SftpHelper::disconnect(ctx);
}

static Napi::Value js_connect(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsObject()) {
		Napi::TypeError::New(env, "Expected an object")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	Napi::Object arg = info[0].As<Napi::Object>();

	std::string host;
	std::string username;
	std::string pubkey;
	std::string privkey;
	std::string password;
	std::string remote_path;
	std::string local_path;

	// ------------------- Mandatory properties --------------------------------
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
		if (!arg.Get("username").IsString() || arg.Get("username").IsEmpty()) {
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

	// ------------------------ Auth properties --------------------------------
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

	SftpWatch_t* ctx  = new SftpWatch_t(env, ++ids);
	ctx->host         = host;
	ctx->username     = username;
	ctx->pubkey       = pubkey;
	ctx->privkey      = privkey;
	ctx->password     = password;
	ctx->remote_path  = remote_path;
	ctx->local_path   = local_path;
	ctx->is_connected = false;

	// -------------------- Optional properties --------------------------------
	if (arg.Has("port")) {
		uint32_t tmp = arg.Get("port").As<Napi::Number>().Uint32Value();
		ctx->port    = static_cast<uint16_t>(tmp);
	}

	if (arg.Has("delayMs")) {
		uint32_t tmp = arg.Get("delayMs").As<Napi::Number>().Uint32Value();
		if (tmp > 0) ctx->delay_ms = tmp;
	}

	if (arg.Has("timeout")) {
		uint32_t tmp = arg.Get("timeout").As<Napi::Number>().Uint32Value();
		if (tmp > 0) ctx->timeout_sec = static_cast<uint16_t>(tmp);
	}

	if (arg.Has("maxErrCount")) {
		uint32_t tmp = arg.Get("maxErrCount").As<Napi::Number>().Uint32Value();
		ctx->max_err_count = static_cast<uint8_t>(tmp);
	}

	if (arg.Has("useKeyboard")) {
		ctx->use_keyboard = arg.Get("useKeyboard").As<Napi::Boolean>().Value();
	}

	if (connect_or_reconnect(ctx)) {
		delete ctx;

		Napi::TypeError::New(env, "Can't connect to sftp server!")
			.ThrowAsJavaScriptException();

		return Napi::Boolean::New(env, false);
	}

	watchers[ctx->id] = ctx;

	return Napi::Number::New(env, ctx->id);
}

static Napi::Value js_sync_dir(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 2) {
		Napi::TypeError::New(
			env, "Invalid params. Should be <context_id, callback>")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[0].IsNumber()) {
		Napi::TypeError::New(env, "Invalid context id. Should be a number")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[1].IsFunction()) {
		Napi::TypeError::New(env, "Invalid Callback Function")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	const uint32_t id    = info[0].As<Napi::Number>().Uint32Value();
	Napi::Function js_cb = info[1].As<Napi::Function>();

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

static Napi::Value js_sync_stop(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1) {
		Napi::TypeError::New(env, "Invalid params. Should be <context_id>")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[0].IsNumber()) {
		Napi::TypeError::New(env, "Invalid context id. Should be a number")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	const uint32_t id = info[0].As<Napi::Number>().Uint32Value();

	SftpWatch_t* ctx = watchers.at(id);
	ctx->is_stopped  = true;

	return env.Undefined();
}

static Napi::Object init_napi(Napi::Env env, Napi::Object exports)
{
	exports.Set(Napi::String::New(env, "connect"),
		Napi::Function::New(env, js_connect));
	exports.Set(
		Napi::String::New(env, "sync"), Napi::Function::New(env, js_sync_dir));
	exports.Set(
		Napi::String::New(env, "stop"), Napi::Function::New(env, js_sync_stop));

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, init_napi);
