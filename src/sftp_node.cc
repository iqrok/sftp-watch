#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>

#include <napi.h>

#include "sftp_helper.hpp"
#include "sftp_node.hpp"
#include "sftp_node_api.hpp"

#define EVT_STR_DEL "del"
#define EVT_STR_NEW "new"
#define EVT_STR_MOD "mod"

#define DELAY_US(us)                                                           \
	std::this_thread::sleep_for(std::chrono::microseconds((us)))
#define DELAY_MS(ms)                                                           \
	std::this_thread::sleep_for(std::chrono::milliseconds((ms)))

#define SNOD_FILE_SIZE_SAME(f1, f2)  ((f1).attrs.filesize == (f2).attrs.filesize)
#define SNOD_FILE_MTIME_SAME(f1, f2) (((f1).attrs.mtime == (f2).attrs.mtime))
#define SNOD_SEC2MS(s)               ((s) * 1000)

enum EventFile_e {
	EVT_FILE_DEL,
	EVT_FILE_NEW,
	EVT_FILE_MOD,
};

/* ************************* Forward Declare ******************************* */
static int32_t connect_or_reconnect(SftpWatch_t* ctx);
static void    sync_dir_js_call(SftpWatch_t* ctx, DirItem_t* file, uint8_t ev);
static int     sync_dir_loop(SftpWatch_t* ctx, RemoteDir_t& dir);
static void    sync_dir_thread(SftpWatch_t* ctx);
static void    sync_dir_finalizer(
	   Napi::Env env, void* finalizeData, SftpWatch_t* ctx);
static void sync_dir_tsfn_cb(
	Napi::Env env, Napi::Function js_cb, SftpWatch_t* ctx);

/*
 * to store watcher contexts.
 * - `ids` is to hold the last set id, always be incremented to avoid same id
 * - `watchers` is to hold all individual contexts itself as a map
 *
 * Keep both as static as the needs must be in this file only. Tracking globals
 * accross files is confusing.
 *
 * NOTE: probably should use vector instead of map
 * */
static uint32_t                         ids = 0;
static std::map<uint32_t, SftpWatch_t*> watchers;

/* ************************* Implementations ******************************* */
static int32_t connect_or_reconnect(SftpWatch_t* ctx)
{
	if (ctx->is_connected) SftpHelper::disconnect(ctx);

	if (SftpHelper::connect(ctx)) return -1;
	if (SftpHelper::auth(ctx)) return -2;

	return 0;
}

static void sync_dir_finalizer(
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

	switch (ctx->ev_file->ev) {

	case EVT_FILE_DEL: {
		obj.Set("evt", Napi::String::New(env, EVT_STR_DEL));
	} break;

	case EVT_FILE_NEW: {
		obj.Set("evt", Napi::String::New(env, EVT_STR_NEW));
	} break;

	case EVT_FILE_MOD: {
		obj.Set("evt", Napi::String::New(env, EVT_STR_MOD));
	} break;

	default:
		break;
	}

	obj.Set("name", Napi::String::New(env, ctx->ev_file->file->name));
	obj.Set("type",
		Napi::String::New(env, std::string(1, ctx->ev_file->file->type)));
	obj.Set("size", Napi::Number::New(env, ctx->ev_file->file->attrs.filesize));
	obj.Set("time",
		Napi::Number::New(env, SNOD_SEC2MS(ctx->ev_file->file->attrs.mtime)));
	obj.Set("perm",
		Napi::Number::New(env, SNOD_FILE_PERM(ctx->ev_file->file->attrs)));

	// don't forget to delete the data, since we used dynamic allocation
	delete ctx->ev_file;

	js_cb.Call({ obj });

	// release the lock
	ctx->sem.release();
}

static void sync_dir_js_call(SftpWatch_t* ctx, DirItem_t* file, uint8_t ev)
{
	// need to use heap, avoiding data lost when race condition occurs
	ctx->ev_file       = new EvtFile_t;
	ctx->ev_file->ev   = ev;
	ctx->ev_file->file = file;

	// BlockingCall() should never fail, since max queue size is 0
	if (ctx->tsfn.BlockingCall(ctx, sync_dir_tsfn_cb) != napi_ok) {
		Napi::Error::Fatal("new file err", "BlockingCall() failed");
	}

	// wait until the BlockingCall is finished
	ctx->sem.acquire();
}

static int sync_dir_loop(SftpWatch_t* ctx, RemoteDir_t& dir)
{
	// we're gonna need pair for the directory. So, create it anyway use []
	PairFileDet_t& tmp = ctx->last_files[dir.path];
	PairFileDet_t  current;
	DirItem_t      item;

	// open remote dir first
	int32_t rc = SftpHelper::open_dir(ctx, &dir);
	if (rc) {
		++ctx->err_count;
		return -1;
	}

	ctx->err_count = 0;

	// read the opened directory
	while ((rc = SftpHelper::read_dir(dir, &item)) != 0) {
		if (item.name.empty()) continue;

		std::string key = item.name;
		current[key]    = item;

		// Check for new or modified files
		bool is_mod = false;
		bool is_new = !tmp.contains(key);

		if (!is_new) {
			is_mod = !SNOD_FILE_SIZE_SAME(tmp.at(key), item)
				|| !SNOD_FILE_MTIME_SAME(tmp.at(key), item);
		}

		if (!is_new && !is_mod) continue;

		tmp[key] = item;

		switch (item.type) {

		case IS_REG_FILE: {
			// TODO: multiple files at once with single connection
			SftpHelper::sync_file_remote(ctx, &item);
		} break;

		// TODO: sync directory and its tree, until reached max depth
		case IS_DIR: {
			SftpHelper::mkdir_local(ctx, &item);

			// TODO: check subdirectory depth before add it into list
			RemoteDir_t sub;
			sub.rela = item.name;

			size_t pos = item.name.find_last_of(SNOD_PATH_SEP);
			if (pos != std::string::npos) {
				sub.path = dir.path + SNOD_SEP + item.name.substr(pos + 1);
			} else {
				sub.path = dir.path + SNOD_SEP + item.name;
			}

			ctx->dirs[item.name] = sub;
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		sync_dir_js_call(ctx, &item, is_new ? EVT_FILE_NEW : EVT_FILE_MOD);
	}

	/*
	 * NOTE: no increment at the end. it will be handled based on condition
	 *       below
	 * */
	for (auto it = tmp.cbegin(); it != tmp.cend();) {
		if (current.contains(it->first)) {
			++it;
			continue;
		}

		DirItem_t old = it->second;

		switch (old.type) {

		case IS_DIR: {
			/*
			 * Pushing back into waiting list to be processed later when dir
			 * loop has been finished
			 * */
			ctx->undirs.push_back(old.name);
		} break;

		default: {
			/*
			 * NOTE: any file type should be safe enough to be remove directly,
			 *       right?
			 * */
			SftpHelper::remove_local(ctx, old.name);
		} break;
		}

		sync_dir_js_call(ctx, &old, EVT_FILE_DEL);

		it = tmp.erase(it);
	}

	// close opened dir
	SftpHelper::close_dir(ctx, &dir);

	return 0;
}

static void sync_dir_thread(SftpWatch_t* ctx)
{
	while (!ctx->is_stopped) {
		for (const auto& [key, val] : ctx->dirs) {
			RemoteDir_t dir = val;
			sync_dir_loop(ctx, dir);
		}

		// check if any directoy is removed
		for (auto it = ctx->undirs.begin(); it != ctx->undirs.end();) {
			/*
			 * Remove local directory recursively and remove the key from remote
			 * dirs map.
			 *
			 * FIXME: There's possibilty that the dir is still iterated once
			 *        more after the deletion from map.
			 *
			 * TODO: check if dir is removed or renamed
			 * */
			SftpHelper::rmdir_local(ctx, *it);
			ctx->dirs.erase(*it);

			// remove the vector itself and go to the next iterator
			it = ctx->undirs.erase(it);
		}

		if (ctx->err_count >= ctx->max_err_count) {
			connect_or_reconnect(ctx);
			ctx->err_count = 0;
		}

		DELAY_MS(ctx->delay_ms);
	}

	// outside loop means no more operation to do. Cleanup the connection
	SftpHelper::disconnect(ctx);
}

Napi::Value js_connect(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsObject()) {
		Napi::TypeError::New(env, "Expected an object")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	Napi::Object arg = info[0].As<Napi::Object>();

	RemoteDir_t dir;
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
		dir.path    = remote_path;
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

	ctx->dirs[ctx->remote_path] = dir;

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

Napi::Value js_sync_dir(const Napi::CallbackInfo& info)
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
			  sync_dir_finalizer, // Finalizer
			  (void*)nullptr      // Finalizer data
		  );

	ctx->thread = std::thread(sync_dir_thread, ctx);

	return Napi::Boolean::New(env, true);
}

Napi::Value js_sync_stop(const Napi::CallbackInfo& info)
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
