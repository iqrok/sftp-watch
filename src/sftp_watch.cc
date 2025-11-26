#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sftp_local.hpp"
#include "sftp_remote.hpp"
#include "sftp_watch.hpp"

#include "debug.hpp"

#define SNOD_PRV_WAIT_MS 50
#define SNOD_THREAD_WAIT(i, sum, fl)                                           \
	do {                                                                       \
		for (uint32_t ms = (i); (fl) && ms <= sum; ms += (i))                  \
			SNOD_DELAY_MS((i));                                                \
	} while (0)

/* ******************** Start of Static Functions *************************** */
namespace {

static bool is_file_same(PathFile_t& list, std::string& key, DirItem_t& item)
{
	if (!list.contains(key)) return false;
	return !SNOD_FILE_IS_DIFF(list.at(key), item);
}

static std::string prv_get_key(std::string root, std::string full)
{
	size_t pos = full.find(root);

	if (pos != std::string::npos) {
		full.erase(pos, root.length());
		return full.empty() ? std::string(SNOD_SEP) : full;
	}

	return full;
}

static void prv_clear_dirs(DirList_t* dirs)
{
	for (auto it = dirs->begin(); it != dirs->end();) {
		if (it->first == SNOD_SEP) {
			++it;
		} else {
			it = dirs->erase(it);
		}
	}

	assert(dirs->size() == 1);
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

static void sync_dir_check_conflict(SftpWatch_t* ctx, SyncQueue_t* que,
	bool& b_path, const std::string& dir, const std::string& path)
{
	/*
	 * Conflict happens when path exists on remote and local snapshots.
	 * When remote and local file is different, remote always wins for now.
	 *
	 * Conflict Resolution is done following this table
	 *
	 * | Local != Base | Remote != Base | Local != Remote |     Operation      |
	 * |---------------|----------------|-----------------|--------------------|
	 * |      0        |       0        |       -         | (Skip. Same Files) |
	 * |      1        |       0        |       -         | Upload             |
	 * |      0        |       1        |       -         | Download           |
	 * |      1        |       1        |       1         | Download           |
	 * |      1        |       1        |       0         | Update Base        |
	 *
	 * */

	/* NOTE: short-circuit OR,
	 *       if left-hand is true, right-hand won't be evaluated.
	 *       So, it's okay if base_snap is still empty and will be added later.
	 * */
	bool lb_diff = !b_path
		|| SNOD_FILE_IS_DIFF(
			ctx->base_snap.at(dir).at(path), ctx->local_snap.at(dir).at(path));
	bool rb_diff = !b_path
		|| SNOD_FILE_IS_DIFF(
			ctx->base_snap.at(dir).at(path), ctx->remote_snap.at(dir).at(path));

	if (!lb_diff && !rb_diff) {
		// skip. both files are the same
		return;
	} else if (lb_diff && !rb_diff) {
		// upload
		ctx->base_snap[dir][path] = ctx->local_snap.at(dir).at(path);
		que->l_new.push_back(&ctx->base_snap[dir][path]);
	} else if (!lb_diff && rb_diff) {
		// download
		ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
		que->r_new.push_back(&ctx->base_snap[dir][path]);
	} else if (lb_diff && rb_diff) {
		bool lr_diff = SNOD_FILE_IS_DIFF(ctx->local_snap.at(dir).at(path),
			ctx->remote_snap.at(dir).at(path));

		// TODO: rule like 'remote-wins' or 'local-wins' could be applied here
		if (lr_diff) {
			// download
			ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
			que->r_new.push_back(&ctx->base_snap[dir][path]);
		} else {
			// actually the base is outdated
			ctx->base_snap[dir][path] = ctx->remote_snap.at(dir).at(path);
		}
	} else {
		// no diff at all. Should be unreachable
		UNREACHABLE_MSG("CONFLICT CHECK DIR '%s' PATH '%s': [%d, %d]\n",
			dir.c_str(), path.c_str(), lb_diff, rb_diff);
	}
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
	 * |     1       |      0       |      0       | (Check Orphans)   |
	 * |     -       |      1       |      1       | (Check Conflict)  |
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
			// NOTE: short-circuit AND. If left is false, right-hand is skipped
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
				// remove base. Should be hanlded on Check Orphans
			} else if (l_path && r_path) {
				// both remote and local exist, check diff
				sync_dir_check_conflict(ctx, que, b_path, dir, path);
			} else {
				// all paths have no diff, Should be unreachable
				UNREACHABLE_MSG("DIR '%s' PATH '%s': [B:L:R %d:%d:%d]\n",
					dir.c_str(), path.c_str(), b_path, l_path, r_path);
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
	for (auto it = que.l_del.begin(); it != que.l_del.end() && !ctx->is_stopped;
		++it) {

		DirItem_t* item = &(*it);

		if (item->type == IS_DIR) {
			ctx->local_dirs.erase(item->name);
			ctx->remote_dirs.erase(item->name);
			SftpRemote::rmdir(ctx, item);
		} else {
			SftpRemote::remove(ctx, item);
		}

		ctx->cb_file(ctx, ctx->user_data, item, true, EVT_FILE_LDEL);
	}

	for (auto it = que.r_del.begin(); it != que.r_del.end() && !ctx->is_stopped;
		++it) {

		DirItem_t* item = &(*it);

		if (item->type == IS_DIR) {
			ctx->local_dirs.erase(item->name);
			ctx->remote_dirs.erase(item->name);
			SftpLocal::rmdir(ctx, item);
		} else {
			SftpLocal::remove(ctx, item);
		}

		ctx->cb_file(ctx, ctx->user_data, item, true, EVT_FILE_RDEL);
	}

	for (auto it = que.r_new.begin(); it != que.r_new.end() && !ctx->is_stopped;
		++it) {

		int32_t rc = 0;

		switch ((*it)->type) {

		case IS_DIR: {
			rc = SftpLocal::mkdir(ctx, (*it));
		} break;

		case IS_SYMLINK: {
			rc = SftpRemote::down_symlink(ctx, *it);
		} break;

		case IS_REG_FILE: {
			ctx->cb_file(ctx, ctx->user_data, (*it), false, EVT_FILE_DOWN);
			rc = SftpRemote::down_file(ctx, *it);
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		if (rc) {
			ctx->last_error.path = (*it)->name.c_str();
			ctx->cb_err(ctx, ctx->user_data, &ctx->last_error);
		}

		ctx->cb_file(ctx, ctx->user_data, (*it), true, EVT_FILE_DOWN);
	}

	for (auto it = que.l_new.begin(); it != que.l_new.end() && !ctx->is_stopped;
		++it) {

		int32_t rc = 0;

		switch ((*it)->type) {

		case IS_REG_FILE: {
			ctx->cb_file(ctx, ctx->user_data, (*it), false, EVT_FILE_UP);
			rc = SftpRemote::up_file(ctx, (*it));
		} break;

		case IS_DIR: {
			rc = SftpRemote::mkdir(ctx, (*it));
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		if (rc) {
			ctx->last_error.path = (*it)->name.c_str();
			ctx->cb_err(ctx, ctx->user_data, &ctx->last_error);
		}

		ctx->cb_file(ctx, ctx->user_data, (*it), true, EVT_FILE_UP);
	}
}

/**
 * @brief check for both local and remote root directories.
 * The root directory must exist and can be opened by the app.
 * */
static bool check_root_dirs(SftpWatch_t* ctx)
{
	int32_t rc = 0;

	LIBSSH2_SFTP_ATTRIBUTES attrs;

	if ((rc = SftpRemote::get_filestat(ctx, ctx->remote_path, &attrs))) {
		ctx->last_error.path = ctx->remote_path.c_str();
		ctx->cb_err(ctx, ctx->user_data, &ctx->last_error);
		return false;
	}

	rc = SftpRemote::open_dir(ctx, &ctx->remote_dirs.at("/"));
	SftpRemote::close_dir(ctx, &ctx->remote_dirs.at("/"));

	if (rc) {
		ctx->last_error.path = ctx->remote_path.c_str();
		ctx->cb_err(ctx, ctx->user_data, &ctx->last_error);
		return false;
	}

	if ((rc = SftpLocal::filestat(ctx, ctx->local_path, &attrs))) {
		ctx->last_error.path = ctx->local_path.c_str();
		ctx->cb_err(ctx, ctx->user_data, &ctx->last_error);
		return false;
	}

	rc = SftpLocal::open_dir(ctx, &ctx->local_dirs.at("/"));
	SftpLocal::close_dir(ctx, &ctx->local_dirs.at("/"));

	if (rc) {
		ctx->last_error.path = ctx->local_path.c_str();
		ctx->cb_err(ctx, ctx->user_data, &ctx->last_error);
		return false;
	}

	return true;
}

/**
 * @brief Main thread to check remote and local directories.
 * */
void sync_thread(SftpWatch_t* ctx)
{
	ctx->is_stopped = !check_root_dirs(ctx);

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
			while (!ctx->is_stopped && SftpWatch::connect_or_reconnect(ctx)) {
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
	ctx->cb_cleanup(ctx, ctx->user_data);
}

} /* ******************** End of Static Functions *************************** */

/* ************************ API Implementations ***************************** */

uint8_t SftpWatch::get_filetype(DirItem_t* file)
{
	if (!(file->attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) return IS_INVALID;

	if (LIBSSH2_SFTP_S_ISREG(file->attrs.permissions)) {
		return IS_REG_FILE;
	} else if (LIBSSH2_SFTP_S_ISDIR(file->attrs.permissions)) {
		return IS_DIR;
	} else if (LIBSSH2_SFTP_S_ISLNK(file->attrs.permissions)) {
		return IS_SYMLINK;
	} else if (LIBSSH2_SFTP_S_ISCHR(file->attrs.permissions)) {
		return IS_CHR_FILE;
	} else if (LIBSSH2_SFTP_S_ISBLK(file->attrs.permissions)) {
		return IS_BLK_FILE;
	} else if (LIBSSH2_SFTP_S_ISFIFO(file->attrs.permissions)) {
		return IS_PIPE;
	} else if (LIBSSH2_SFTP_S_ISSOCK(file->attrs.permissions)) {
		return IS_SOCK;
	} else {
		return IS_INVALID;
	}
}

int32_t SftpWatch::set_user_data(SftpWatch_t* ctx, UserData_t data)
{
	if (!ctx || !data) return -1;

	ctx->user_data = data;

	return 0;
}

int32_t SftpWatch::connect_or_reconnect(SftpWatch_t* ctx)
{
	SftpRemote::disconnect(ctx);

	if (SftpRemote::connect(ctx)) return -1;

	if (SftpRemote::auth(ctx)) return -2;

	return 0;
}

void SftpWatch::start(SftpWatch_t* ctx)
{
	ctx->is_stopped = false;
	ctx->thread     = std::thread(sync_thread, ctx);
}

void SftpWatch::request_stop(SftpWatch_t* ctx)
{
	ctx->is_stopped = true;
}

void SftpWatch::disconnect(SftpWatch_t* ctx)
{
	SftpRemote::disconnect(ctx);
}

void SftpWatch::clear(SftpWatch_t* ctx)
{
	ctx->base_snap.clear();
	ctx->local_snap.clear();
	ctx->remote_snap.clear();

	prv_clear_dirs(&ctx->remote_dirs);
	prv_clear_dirs(&ctx->local_dirs);

	ctx->err_count = 0;
}

uint8_t SftpWatch::status(SftpWatch_t* ctx)
{
	return static_cast<uint8_t>(ctx->status);
}
