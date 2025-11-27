#ifndef _SFTP_NODE_API_HPP
#define _SFTP_NODE_API_HPP

#include <napi.h>

#include "sftp_watch.hpp"

typedef struct StopWorker_s StopWorker_t;
typedef struct EvtFile_s    EvtFile_t;

class SftpNode : public Napi::ObjectWrap<SftpNode> {
public:
	std::string       id         = "";
	std::atomic<bool> is_running = false;

	std::binary_semaphore sem_main;

	Napi::ThreadSafeFunction tsfn_sync = nullptr;
	std::binary_semaphore    sem_sync;

	Napi::ThreadSafeFunction tsfn_err = nullptr;
	std::binary_semaphore    sem_err;
	Napi::ObjectReference    obj_err;

	static void tsfn_sync_finalizer(
		Napi::Env env, SftpNode* data, SftpWatch_t* ctx);
	static void tsfn_sync_cb(
		Napi::Env env, Napi::Function js_cb, SftpNode* node_ctx);
	static void tsfn_sync_js_call(SftpWatch_t* ctx, UserData_t data,
		DirItem_t* file, bool status, EventFile_t ev);

	static void tsfn_err_cb(
		Napi::Env env, Napi::Function js_cb, SftpNode* node_ctx);
	static void tsfn_err_js_call(
		SftpWatch_t* ctx, UserData_t data, SyncErr_t* error);

	static void stop_finalizer(Napi::Env env, void* data, StopWorker_t* stop);
	static void stop_js_call(
		Napi::Env env, Napi::Function js_cb, SftpNode* node_ctx);
	static void stop_execute(StopWorker_t* stop, SftpNode* node_ctx);

	static void thread_cleanup(SftpWatch_t* ctx, UserData_t data);

	Napi::ObjectReference* create_obj_error(Napi::Env env, SyncErr_t* error);

	SftpNode(const Napi::CallbackInfo& info);
	~SftpNode();

	void         cleanup();
	SftpWatch_t* get_watch_ctx();
	EvtFile_t*   set_file_event(EventFile_t& ev, bool& status, DirItem_t* file);
	EvtFile_t*   get_file_event();
	void         delete_file_event();

	Napi::Value connect(const Napi::CallbackInfo& info);
	Napi::Value sync_start(const Napi::CallbackInfo& info);
	Napi::Value sync_stop(const Napi::CallbackInfo& info);
	Napi::Value listen_to(const Napi::CallbackInfo& info);
	Napi::Value get_error(const Napi::CallbackInfo& info);
	Napi::Value fingerprint(const Napi::CallbackInfo& info);

	StopWorker_t* stop       = nullptr;
	SyncErr_t*    last_error = nullptr;

private:
	SftpWatch_t* ctx = nullptr;
	EvtFile_t*   ev_file; /**< pointer to event data for js callback */
};

struct StopWorker_s {
	Napi::Promise::Deferred  deferred;
	Napi::ThreadSafeFunction tsfn;
	SftpNode*                node_ctx;
	std::thread              thread;
	std::binary_semaphore    sem_stop;
	std::atomic<bool>        is_requested;

	StopWorker_s(const Napi::CallbackInfo& info, SftpNode* node_ctx)
		: deferred(Napi::Promise::Deferred::New(info.Env()))
		, node_ctx(node_ctx)
		, sem_stop(0)
		, is_requested(false)
	{
		// empty constructor
	}
};

struct EvtFile_s {
	bool       status;
	uint8_t    ev;
	DirItem_t* file;
};

#endif
