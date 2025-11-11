#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <napi.h>

#include "sftp_local.hpp"
#include "sftp_node.hpp"
#include "sftp_node_api.hpp"
#include "sftp_remote.hpp"

struct EvtFile_s {
	bool       status = false;
	uint8_t    ev;
	DirItem_t* file;
};

#define SNOD_PRV_WAIT_MS 50
#define SNOD_THREAD_WAIT(i, sum, fl)                                           \
	do {                                                                       \
		for (uint32_t ms = (i); (fl) && ms <= sum; ms += (i))                  \
			SNOD_DELAY_MS((i));                                                \
	} while (0)

namespace { // start of unnamed namespace for static function

typedef std::map<std::string, std::unordered_set<std::string>> AllIns_t;

/* ************************* Forward Declare ******************************* */
static int32_t connect_or_reconnect(SftpWatch_t* ctx);
static void    sync_dir_js_call(SftpWatch_t* ctx, DirItem_t* file, uint8_t ev);
static int     sync_dir_remote(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins);
static int     sync_dir_local(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins);
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
 * NOTE: using unordered_map as it's should be small enough and access is
 *       generally faster.
 * */
static uint32_t                                   ids = 0;
static std::unordered_map<uint32_t, SftpWatch_t*> watchers;

/* ************************* Implementations ******************************* */
static int32_t connect_or_reconnect(SftpWatch_t* ctx)
{
	if (ctx->is_connected) SftpRemote::disconnect(ctx);

	if (SftpRemote::connect(ctx)) return -1;
	if (SftpRemote::auth(ctx)) return -2;

	return 0;
}

static void sync_dir_finalizer(
	Napi::Env env, void* finalizeData, SftpWatch_t* ctx)
{
	(void)finalizeData; // unused. btw it's a nullptr

	SftpRemote::disconnect(ctx);

	ctx->thread.join();
	ctx->deferred.Resolve(Napi::Number::New(env, ctx->id));

	watchers.erase(ctx->id);

	if (watchers.empty()) SftpRemote::shutdown();

	delete ctx;
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

	case EVT_FILE_REN: {
		obj.Set("evt", Napi::String::New(env, EVT_STR_REN));
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

std::string prv_get_key(std::string root, std::string full)
{
	size_t pos = full.find(root);

	if (pos != std::string::npos) {
		full.erase(pos, root.length());
		return full.empty() ? std::string(SNOD_SEP) : full;
	}

	return full;
}

//~ void prv_print_tree(DirSnapshot_t& list)
//~ {
	//~ // get PathFile_t from list
	//~ for (auto& [key, dir] : list) {
		//~ printf("'%s' =>\n", key.c_str());

		//~ // get DirItem_t from list
		//~ for (auto& [path, file] : dir) {
			//~ printf("  '%s' - '%s' <type %c> {%llu bytes} %lo\n", path.c_str(), file.name.c_str(), file.type, file.attrs.filesize, file.attrs.permissions);
		//~ }
	//~ }
	//~ printf("============================================\n");
//~ }

static int sync_dir_local(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins)
{
	std::string snap_key = prv_get_key(ctx->local_path, dir.path);
	PathFile_t& list = ctx->local_snap[snap_key];

	// use set to store current key. No need to store the item
	std::unordered_set<std::string> current;

	DirItem_t item;
	int32_t   rc;

	// open local dir first
	if ((rc = SftpLocal::open_dir(ctx, &dir))) {
		return -1;
	}

	ins->insert({ snap_key, {} });

	// read the opened directory
	while ((rc = SftpLocal::read_dir(dir, &item))) {
		if (item.name.empty()) continue;

		std::string key = item.name;
		current.insert(key);

		// Check for new or modified files
		bool is_mod = false;
		bool is_new = !list.contains(key);

		if (!is_new) is_mod = SNOD_FILE_IS_DIFF(list.at(key), item);
		if (!is_new && !is_mod) continue;

		list[key] = item;
		ins->at(snap_key).insert(key);

		switch (item.type) {

		case IS_DIR: {
			SftpLocal::mkdir(ctx, &item);

			// TODO: check subdirectory depth before add it into list
			Directory_t sub;
			sub.rela = item.name;

			size_t pos = item.name.find_last_of(SNOD_SEP_CHAR);
			if (pos != std::string::npos) {
				sub.path = dir.path + SNOD_SEP + item.name.substr(pos + 1);
			} else {
				sub.path = dir.path + SNOD_SEP + item.name;
			}

			ctx->local_dirs[item.name] = sub;
		} break;

		default: {
			/*
			 * NOTE: to avoid double copy, use pointer of item saved in `list`.
			 *       `list` has lifetime long enough until downloads are done.
			 * */
			ctx->uploads.push_back(&list.at(key));
		} break;
		}
		//~ printf("'%s' <type %c> %llu bytes %lo | %lu\n", item.name.c_str(), item.type, item.attrs.filesize, item.attrs.permissions, item.attrs.mtime);
	}

	for (auto it = list.begin(); it != list.end();) {
		if (current.find(it->first) != current.end()) {
			++it;
			continue;
		}

		DirItem_t* old = &it->second;

		//~ switch (old->type) {

		//~ case IS_DIR: {
			//~ /*
			 //~ * Pushing back into waiting list to be processed later when dir
			 //~ * loop has been finished
			 //~ * */
			//~ ctx->remote_undirs.push_back(old->name);
		//~ } break;

		//~ default: {
			//~ /*
			 //~ * NOTE: any file type should be safe enough to be remove directly,
			 //~ *       right?
			 //~ * */
			//~ SftpLocal::remove(ctx, old);
		//~ } break;
		//~ }

		list.erase(snap_key);
		ins->at(snap_key).insert(old->name);
		//~ ins->path.insert(old->name);

		//~ sync_dir_js_call(ctx, old, EVT_FILE_DEL);

		it = list.erase(it);
	}

	// close opened dir
	SftpLocal::close_dir(ctx, &dir);

	return 0;
}

static int sync_dir_remote(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins)
{
	std::string snap_key = prv_get_key(ctx->remote_path, dir.path);
	// we're gonna need pair for the directory. So, create it anyway use []
	PathFile_t& list = ctx->remote_snap[snap_key];

	// use set to store current key. No need to store the item
	std::unordered_set<std::string> current;

	DirItem_t item;
	int32_t   rc;

	// open remote dir first
	if ((rc = SftpRemote::open_dir(ctx, &dir))) {
		++ctx->err_count;
		return -1;
	}

	//~ ins->dir.insert(snap_key);
	ins->insert({ snap_key, {} });
	ctx->err_count = 0;

	// read the opened directory
	while ((rc = SftpRemote::read_dir(dir, &item))) {
		if (item.name.empty()) continue;

		std::string key = item.name;
		current.insert(key);

		// Check for new or modified files
		bool is_mod = false;
		bool is_new = !list.contains(key);

		if (!is_new) is_mod = SNOD_FILE_IS_DIFF(list.at(key), item);

		if (!is_new && !is_mod) continue;

		list[key] = item;
		//~ ins->path.insert(key);
		ins->at(snap_key).insert(key);

		switch (item.type) {

		case IS_DIR: {
			SftpLocal::mkdir(ctx, &item);

			// TODO: check subdirectory depth before add it into list
			Directory_t sub;
			sub.rela = item.name;

			size_t pos = item.name.find_last_of(SNOD_SEP_CHAR);
			if (pos != std::string::npos) {
				sub.path = dir.path + SNOD_SEP + item.name.substr(pos + 1);
			} else {
				sub.path = dir.path + SNOD_SEP + item.name;
			}

			ctx->remote_dirs[item.name] = sub;
		} break;

		default: {
			/*
			 * NOTE: to avoid double copy, use pointer of item saved in `list`.
			 *       `list` has lifetime long enough until downloads are done.
			 * */
			ctx->downloads.push_back(&list.at(key));
		} break;
		}

		sync_dir_js_call(ctx, &item, is_new ? EVT_FILE_NEW : EVT_FILE_MOD);
	}

	/*
	 * Removal of orphaned directory items.
	 *
	 * NOTE: no increment at the end. it will be handled based on condition
	 *       below
	 * */
	for (auto it = list.begin(); it != list.end();) {
		if (current.find(it->first) != current.end()) {
			++it;
			continue;
		}

		DirItem_t* old = &it->second;

		switch (old->type) {

		case IS_DIR: {
			/*
			 * Pushing back into waiting list to be processed later when dir
			 * loop has been finished
			 * */
			ctx->remote_undirs.push_back(old->name);
		} break;

		default: {
			/*
			 * NOTE: any file type should be safe enough to be remove directly,
			 *       right?
			 * */
			SftpLocal::remove(ctx, old);
		} break;
		}

		list.erase(snap_key);
		ins->at(snap_key).insert(old->name);
		//~ ins->path.insert(old->name);

		sync_dir_js_call(ctx, old, EVT_FILE_DEL);

		it = list.erase(it);
	}

	// close opened dir
	SftpRemote::close_dir(ctx, &dir);

	return 0;
}

static void compare_snapshots(SftpWatch_t* ctx, AllIns_t& ins)
{
	uint32_t cnt = 0;

	for (const auto& [dir, lpath] : ins) {
		bool b_dir = ctx->base_snap.contains(dir);
		bool l_dir = ctx->local_snap.contains(dir);
		bool r_dir = ctx->remote_snap.contains(dir);

		for (const auto& path : lpath) {
			bool b_path = b_dir && ctx->base_snap.at(dir).contains(path);
			bool l_path = l_dir && ctx->local_snap.at(dir).contains(path);
			bool r_path = r_dir && ctx->remote_snap.at(dir).contains(path);

			printf("%d: DIR '%s' PATH '%s' [%d, %d, %d]\n", ++cnt, dir.c_str(), path.c_str(), b_path, l_path, r_path);

			if (!b_path && !l_path && r_path) {
				// download
				printf("DOWNLOAD '%s'\n", path.c_str());
				ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
			} else if (!b_path && l_path && !r_path) {
				// upload
				printf("UPLOAD   '%s'\n", path.c_str());
				ctx->base_snap[dir][path] = ctx->local_snap.at(dir).at(path);
			} else if (b_path && l_path && !r_path) {
				// remove local
				printf("DEL LOCAL '%s'\n", path.c_str());
				ctx->base_snap.at(dir).erase(path);
			} else if (b_path && !l_path && r_path) {
				// remove remote
				printf("DEL REMOTE '%s'\n", path.c_str());
				ctx->base_snap.at(dir).erase(path);
			} else if (b_path && !l_path && !r_path) {
				// remove base
				printf("DEL BASE '%s'\n", path.c_str());
				ctx->base_snap.at(dir).erase(path);
			} else if (b_path && l_path && r_path) {
				bool lb_diff = SNOD_FILE_IS_DIFF(ctx->base_snap.at(dir).at(path), ctx->local_snap.at(dir).at(path));
				bool rb_diff = SNOD_FILE_IS_DIFF(ctx->base_snap.at(dir).at(path), ctx->remote_snap.at(dir).at(path));

				if (!lb_diff && !rb_diff) continue;

				if (lb_diff && !rb_diff) {
					// upload
					printf("UPLOAD MOD   '%s'\n", path.c_str());
					ctx->base_snap[dir][path] = ctx->local_snap.at(dir).at(path);
				} else if (!lb_diff && rb_diff) {
					// download
					printf("DOWNLOAD MOD '%s'\n", path.c_str());
					ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
				} else if (lb_diff && rb_diff) {
					bool lr_diff = SNOD_FILE_IS_DIFF(ctx->local_snap.at(dir).at(path), ctx->remote_snap.at(dir).at(path));
					if (lr_diff) {
						// download
						printf("DOWNLOAD MOD 2 '%s'\n", path.c_str());
						ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
					}
				} else {
					// no diff at all. Should not be reached
				}
			} else {
				// all paths have no diff, continue
			}
		}
	}

	// TODO: Erase empty base
}

static void sync_dir_thread(SftpWatch_t* ctx)
{
	while (!ctx->is_stopped) {
		int32_t  rc = 0;
		AllIns_t ins;

		for (auto& [key, dir] : ctx->local_dirs) {
			if (ctx->is_stopped || (rc = sync_dir_local(ctx, dir, &ins))) break;
		}

		for (auto& [key, dir] : ctx->remote_dirs) {
			if (ctx->is_stopped || (rc = sync_dir_remote(ctx, dir, &ins))) break;
		}

		compare_snapshots(ctx, ins);

		//~ printf("==================== LOCAL SNAPSHOT ==========================\n");
		//~ prv_print_tree(ctx->local_snap);

		//~ printf("=================== REMOTE SNAPSHOT ==========================\n");
		//~ prv_print_tree(ctx->remote_snap);

		for (auto it = ctx->downloads.begin(); it != ctx->downloads.end();) {
			if (ctx->is_stopped) {
				ctx->downloads.clear();
				break;
			}

			switch ((*it)->type) {

			case IS_SYMLINK: {
				SftpRemote::copy_symlink(ctx, *it);
			} break;

			case IS_REG_FILE: {
				SftpRemote::copy_file(ctx, *it);
			} break;

			default: {
				// nothing to do for now
			} break;
			}

			// remove the vector itself and go to the next iterator
			it = ctx->downloads.erase(it);
		}

		/*
		 * Remove local directory recursively and remove the key from remote
		 * remote_dirs map.
		 *
		 * FIXME: There's possibilty that the dir is still iterated once
		 *        more after the deletion from map.
		 *
		 * TODO: check if dir is removed or renamed
		 * */
		for (auto it = ctx->remote_undirs.begin();
			it != ctx->remote_undirs.end();) {

			SftpLocal::rmdir(ctx, *it);
			ctx->remote_dirs.erase(*it);
			//~ ctx->remote_snap.erase(SNOD_SEP + (*it));

			// remove the vector itself and go to the next iterator
			it = ctx->remote_undirs.erase(it);
		}

		if (ctx->err_count >= ctx->max_err_count && !ctx->is_stopped) {
			int16_t reconnect_delay = ctx->delay_ms;
			while (!ctx->is_stopped && connect_or_reconnect(ctx)) {
				if (reconnect_delay < ctx->timeout_sec) {
					reconnect_delay += ctx->delay_ms;
				}
				SNOD_DELAY_MS(reconnect_delay);
			}
			ctx->err_count = 0;
		}

		SNOD_THREAD_WAIT(SNOD_PRV_WAIT_MS, ctx->delay_ms, !ctx->is_stopped);
	}

	// Cleaning up
	ctx->tsfn.Release();
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

	Directory_t remote_dir;
	Directory_t local_dir;
	std::string host;
	std::string username;
	std::string pubkey;
	std::string privkey;
	std::string password;

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

		remote_dir.path = arg.Get("remotePath").As<Napi::String>().Utf8Value();
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

		local_dir.path = arg.Get("localPath").As<Napi::String>().Utf8Value();
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

	// ------------------------ Init context -----------------------------------
	SftpWatch_t* ctx = new SftpWatch_t(env, ++ids, host, username, pubkey,
		privkey, password, remote_dir, local_dir);

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

	return ctx->deferred.Promise();
}

Napi::Value js_sync_stop(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1) {
		Napi::TypeError::New(env, "Invalid params. Should be <context_id>")
			.ThrowAsJavaScriptException();
		return env.Undefined();
	}

	if (!info[0].IsNumber()) {
		Napi::TypeError::New(env, "Invalid context id. Should be a number")
			.ThrowAsJavaScriptException();
		return env.Undefined();
	}

	const uint32_t id = info[0].As<Napi::Number>().Uint32Value();

	if (!watchers.contains(id)) return Napi::Boolean::New(env, true);

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

} // end of unnamed namespace for static function
