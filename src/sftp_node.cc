#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

namespace { // start of unnamed namespace for static function

/* ************************* Forward Declare ******************************* */

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
//~ static uint32_t                                   ids = 0;
//~ static std::unordered_map<uint32_t, SftpWatch_t*> watchers;

} // end of unnamed namespace for static function

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

int32_t SyncDir::connect_or_reconnect(SftpWatch_t* ctx)
{
	if (ctx->is_connected) SftpRemote::disconnect(ctx);

	if (SftpRemote::connect(ctx)) return -1;
	if (SftpRemote::auth(ctx)) return -2;

	return 0;
}

void SyncDir::disconnect(SftpWatch_t* ctx)
{
	SftpRemote::disconnect(ctx);
}

int SyncDir::sync_dir_local(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins)
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

int SyncDir::sync_dir_remote(SftpWatch_t* ctx, Directory_t& dir, AllIns_t* ins)
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

void sync_dir_cmp_snap(SftpWatch_t* ctx, AllIns_t& ins, SyncQueue_t* que)
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

void SyncDir::sync_dir_op(SftpWatch_t* ctx, SyncQueue_t& que)
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

		switch ((*it)->type) {

		case IS_DIR: {
			SftpLocal::mkdir(ctx, (*it));
		} break;

		case IS_SYMLINK: {
			SftpRemote::down_symlink(ctx, *it);
		} break;

		case IS_REG_FILE: {
			ctx->cb_file(ctx, ctx->user_data, (*it), false, EVT_FILE_DOWN);
			SftpRemote::down_file(ctx, *it);
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		ctx->cb_file(ctx, ctx->user_data, (*it), true, EVT_FILE_DOWN);
	}

	for (auto it = que.l_new.begin(); it != que.l_new.end() && !ctx->is_stopped;
		++it) {

		switch ((*it)->type) {

		case IS_REG_FILE: {
			ctx->cb_file(ctx, ctx->user_data, (*it), false, EVT_FILE_UP);
			SftpRemote::up_file(ctx, (*it));
		} break;

		case IS_DIR: {
			SftpRemote::mkdir(ctx, (*it));
		} break;

		default: {
			// nothing to do for now
		} break;
		}

		ctx->cb_file(ctx, ctx->user_data, (*it), true, EVT_FILE_UP);
	}
}

void SyncDir::sync_dir_thread(SftpWatch_t* ctx)
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
	//~ ctx->tsfn.Release();
	ctx->cb_cleanup(ctx, ctx->user_data);
}
