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

#define SNOD_PRV_WAIT_MS 50
#define SNOD_THREAD_WAIT(i, sum, fl)                                           \
	do {                                                                       \
		for (uint32_t ms = (i); (fl) && ms <= sum; ms += (i))                  \
			SNOD_DELAY_MS((i));                                                \
	} while (0)

struct EvtFile_s {
	bool       status = false;
	uint8_t    ev;
	DirItem_t* file;
};

namespace { // start of unnamed namespace for static function

enum EventFile_e {
	EVT_FILE_LDEL = 0x00,
	EVT_FILE_UP   = 0x01,
	EVT_FILE_RDEL = 0x02,
	EVT_FILE_DOWN = 0x03,
};

typedef std::map<std::string, std::unordered_set<std::string>> AllIns_t;

typedef struct SyncQueue_s {
	std::vector<DirItem_t*> l_new;
	std::vector<DirItem_t*> r_new;
	std::vector<DirItem_t>  r_del;
	std::vector<DirItem_t>  l_del;
} SyncQueue_t;

/* ************************* Forward Declare ******************************* */
static int32_t connect_or_reconnect(SftpWatch_t* ctx);
static void    sync_dir_js_call(SftpWatch_t* ctx, DirItem_t* file, uint8_t ev);
static int  sync_dir_remote(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins);
static int  sync_dir_local(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins);
static void sync_dir_thread(SftpWatch_t* ctx);
static void sync_dir_finalizer(
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
static bool is_file_same(PathFile_t& list, std::string& key, DirItem_t& item)
{
	if (!list.contains(key)) return false;
	return !SNOD_FILE_IS_DIFF(list.at(key), item);
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

	case EVT_FILE_RDEL: {
		obj.Set("evt", Napi::String::New(env, "delR"));
	} break;

	case EVT_FILE_LDEL: {
		obj.Set("evt", Napi::String::New(env, "delL"));
	} break;

	case EVT_FILE_UP: {
		obj.Set("evt", Napi::String::New(env, "up"));
	} break;

	case EVT_FILE_DOWN: {
		obj.Set("evt", Napi::String::New(env, "down"));
	} break;

	default:{
	} break;
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

static int sync_dir_local(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins)
{
	std::string snap_key = prv_get_key(ctx->local_path, dir.path);
	PathFile_t& list     = ctx->local_snap[snap_key];
	DirList_t&  dirs     = ctx->local_dirs;

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

		std::string& key = item.name;
		current.insert(key);

		// Check for new or modified files
		if (is_file_same(list, key, item)) continue;

		list[key] = item;
		ins->at(snap_key).insert(key);

		if (item.type == IS_DIR) {
			// TODO: check subdirectory depth before add it into list
			std::string parent_key = (dir.rela == "") ? "/" : dir.rela;

			Directory_t sub;
			sub.rela  = item.name;
			sub.depth = dirs.at(parent_key).depth + 1;

			size_t pos = item.name.find_last_of(SNOD_SEP_CHAR);
			if (pos != std::string::npos) {
				sub.path = dir.path + SNOD_SEP + item.name.substr(pos + 1);
			} else {
				sub.path = dir.path + SNOD_SEP + item.name;
			}

			dirs[item.name] = sub;
		}
	}

	for (auto it = list.begin(); it != list.end();) {
		if (current.find(it->first) != current.end()) {
			++it;
			continue;
		}

		list.erase(snap_key);
		ins->at(snap_key).insert(it->second.name);

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
	PathFile_t& list     = ctx->remote_snap[snap_key];
	DirList_t&  dirs     = ctx->remote_dirs;

	// use set to store current key. No need to store the item
	std::unordered_set<std::string> current;

	DirItem_t item;
	int32_t   rc;

	// open remote dir first
	if ((rc = SftpRemote::open_dir(ctx, &dir))) {
		++ctx->err_count;
		return -1;
	}

	ins->insert({ snap_key, {} });
	ctx->err_count = 0;

	// read the opened directory
	while ((rc = SftpRemote::read_dir(dir, &item))) {
		if (item.name.empty()) continue;

		std::string& key = item.name;
		current.insert(key);

		// Check for new or modified files
		if (is_file_same(list, key, item)) continue;

		list[key] = item;
		ins->at(snap_key).insert(key);

		if (item.type == IS_DIR) {
			// TODO: check subdirectory depth before add it into list
			std::string parent = (dir.rela == "") ? SNOD_SEP : dir.rela;

			Directory_t sub;
			sub.rela  = item.name;
			sub.depth = dirs.at(parent).depth + 1;

			size_t pos = item.name.find_last_of(SNOD_SEP_CHAR);
			if (pos != std::string::npos) {
				sub.path = dir.path + SNOD_SEP + item.name.substr(pos + 1);
			} else {
				sub.path = dir.path + SNOD_SEP + item.name;
			}

			dirs[item.name] = sub;
		}
	}

	for (auto it = list.begin(); it != list.end();) {
		if (current.find(it->first) != current.end()) {
			++it;
			continue;
		}

		DirItem_t* old = &it->second;

		list.erase(snap_key);
		ins->at(snap_key).insert(old->name);

		it = list.erase(it);
	}

	// close opened dir
	SftpRemote::close_dir(ctx, &dir);

	return 0;
}

static void sync_dir_cmp_snap(SftpWatch_t* ctx, AllIns_t& ins, SyncQueue_t* que)
{
	/*
	 * Compare snapshots to perform 3-way merge. Base snapshot is used as anchor
	 * To select which operation would be done, will follow this table
	 *
	 * | Base Exists | Local Exists | Remote Exist |     Operation     |
	 * |-------------|--------------|--------------|-------------------|
	 * |     0       |      0       |      1       | Download          |
	 * |     0       |      1       |      0       | Upload            |
	 * |     1       |      1       |      0       | Delete local      |
	 * |     1       |      0       |      1       | Delete remote     |
	 * |     1       |      0       |      0       | (Check Orphans)    |
	 * |     1       |      1       |      1       | (Check Conflict)  |
	 *
	 * Conflict happens when path exists on all snapshots. When remote and local
	 * file is different, remote always wins.
	 *
	 * Conflict Resolution is done following this table
	 *
	 * | Local != Base | Remote != Base | Local != Remote |     Operation     |
	 * |---------------|----------------|-----------------|-------------------|
	 * |      1        |       0        |       -         | Upload            |
	 * |      0        |       1        |       -         | Download          |
	 * |      1        |       1        |       1         | Download          |
	 * |      1        |       1        |       0         | Update Base       |
	 *
	 * Orphaned Item is defined as a path whose parent directory no longer
	 * exists both in remote and local. Orphaned items will be removed from
	 * all snapshots.
	 * */
	std::unordered_set<std::string> walked_dir;

	for (const auto& [dir, lpath] : ins) {
		walked_dir.insert(dir);
		bool b_dir = ctx->base_snap.contains(dir);
		bool l_dir = ctx->local_snap.contains(dir);
		bool r_dir = ctx->remote_snap.contains(dir);

		for (const auto& path : lpath) {
			bool b_path = b_dir && ctx->base_snap.at(dir).contains(path);
			bool l_path = l_dir && ctx->local_snap.at(dir).contains(path);
			bool r_path = r_dir && ctx->remote_snap.at(dir).contains(path);

			if (!b_path && !l_path && r_path) {
				// download
				ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
				que->r_new.push_back(&ctx->base_snap[dir][path]);
			} else if (!b_path && l_path && !r_path) {
				// upload
				ctx->base_snap[dir][path] = ctx->local_snap.at(dir).at(path);
				que->l_new.push_back(&ctx->base_snap[dir][path]);
			} else if (b_path && l_path && !r_path) {
				// remote removed
				que->r_del.push_back(ctx->base_snap.at(dir).at(path));
				ctx->base_snap.at(dir).erase(path);
				ctx->remote_snap.at(dir).erase(path);
				ctx->local_snap.at(dir).erase(path);
			} else if (b_path && !l_path && r_path) {
				// local removed
				que->l_del.push_back(ctx->base_snap.at(dir).at(path));
				ctx->base_snap.at(dir).erase(path);
				ctx->remote_snap.at(dir).erase(path);
				ctx->local_snap.at(dir).erase(path);
			} else if (b_path && !l_path && !r_path) {
				// remove base
			} else if (b_path && l_path && r_path) {
				bool lb_diff
					= SNOD_FILE_IS_DIFF(ctx->base_snap.at(dir).at(path),
						ctx->local_snap.at(dir).at(path));
				bool rb_diff
					= SNOD_FILE_IS_DIFF(ctx->base_snap.at(dir).at(path),
						ctx->remote_snap.at(dir).at(path));

				if (!lb_diff && !rb_diff) continue;

				if (lb_diff && !rb_diff) {
					// upload
					ctx->base_snap[dir][path]
						= ctx->local_snap.at(dir).at(path);
					que->l_new.push_back(&ctx->base_snap[dir][path]);
				} else if (!lb_diff && rb_diff) {
					// download
					ctx->base_snap[dir][path]
						= ctx->remote_snap.at(dir).at(path);
					que->r_new.push_back(&ctx->base_snap[dir][path]);
				} else if (lb_diff && rb_diff) {
					bool lr_diff
						= SNOD_FILE_IS_DIFF(ctx->local_snap.at(dir).at(path),
							ctx->remote_snap.at(dir).at(path));
					if (lr_diff) {
						// download
						ctx->base_snap[dir][path]
							= ctx->remote_snap.at(dir).at(path);
						que->r_new.push_back(&ctx->base_snap[dir][path]);
					} else {
						// actually the base is outdated
						ctx->base_snap[dir][path]
							= ctx->remote_snap.at(dir).at(path);
					}
				} else {
					// no diff at all. Should not be reached
				}
			} else {
				// all paths have no diff, continue
			}
		}
	}

	// Check for orphaned item in base snapshot
	for (auto it = ctx->base_snap.begin(); it != ctx->base_snap.end();) {
		const std::string& dir      = it->first;
		PathFile_t&        contents = it->second;

		if (walked_dir.contains(dir)) {
			++it;
			continue;
		}

		for (auto& [path, item] : contents) {
			que->r_del.push_back(ctx->local_snap.at(dir).at(path));
			que->l_del.push_back(ctx->remote_snap.at(dir).at(path));

			ctx->local_snap.at(dir).erase(path);
			ctx->remote_snap.at(dir).erase(path);
		}

		ctx->local_snap.erase(dir);
		ctx->remote_snap.erase(dir);
		it = ctx->base_snap.erase(it);
	}
}

static void sync_dir_op(SftpWatch_t* ctx, SyncQueue_t& que)
{

	for (auto it = que.l_del.begin(); it != que.l_del.end(); ++it) {
		if (ctx->is_stopped) break;

		DirItem_t* item = &(*it);

		if (item->type == IS_DIR) {
			ctx->local_dirs.erase(item->name);
			ctx->remote_dirs.erase(item->name);
			SftpRemote::rmdir(ctx, item);
		} else {
			SftpRemote::remove(ctx, item);
		}

		sync_dir_js_call(ctx, item, EVT_FILE_LDEL);
	}

	for (auto it = que.r_del.begin(); it != que.r_del.end(); ++it) {
		if (ctx->is_stopped) break;

		DirItem_t* item = &(*it);

		if (item->type == IS_DIR) {
			ctx->local_dirs.erase(item->name);
			ctx->remote_dirs.erase(item->name);
			SftpLocal::rmdir(ctx, item);
		} else {
			SftpLocal::remove(ctx, item);
		}

		sync_dir_js_call(ctx, item, EVT_FILE_RDEL);
	}

	for (auto it = que.r_new.begin(); it != que.r_new.end(); ++it) {

		switch ((*it)->type) {

		case IS_DIR: {
			SftpLocal::mkdir(ctx, (*it));
		} break;

		case IS_SYMLINK: {
			SftpRemote::down_symlink(ctx, *it);
		} break;

		case IS_REG_FILE: {
			SftpRemote::down_file(ctx, *it);
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		sync_dir_js_call(ctx, (*it), EVT_FILE_DOWN);
	}

	for (auto it = que.l_new.begin(); it != que.l_new.end(); ++it) {

		switch ((*it)->type) {

		case IS_REG_FILE: {
			SftpRemote::up_file(ctx, (*it));
		} break;

		case IS_DIR: {
			SftpRemote::mkdir(ctx, (*it));
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		sync_dir_js_call(ctx, (*it), EVT_FILE_UP);
	}
}

static void sync_dir_thread(SftpWatch_t* ctx)
{
	while (!ctx->is_stopped) {
		int32_t     rc = 0;
		AllIns_t    ins;
		SyncQueue_t que;

		for (auto& [key, dir] : ctx->local_dirs) {
			if (ctx->is_stopped || (rc = sync_dir_local(ctx, dir, &ins))) {
				break;
			}
		}

		for (auto& [key, dir] : ctx->remote_dirs) {
			if (ctx->is_stopped || (rc = sync_dir_remote(ctx, dir, &ins))) {
				break;
			}
		}

		sync_dir_cmp_snap(ctx, ins, &que);
		sync_dir_op(ctx, que);

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

	// Cleanup
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
