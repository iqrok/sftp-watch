#ifndef _SFTP_NODE_API_HPP
#define _SFTP_NODE_API_HPP

#include <napi.h>

#include "sftp_watch.hpp"

typedef struct EvtFile_s {
	bool       status;
	uint8_t    ev;
	DirItem_t* file;
} EvtFile_t;

class SftpNode : public Napi::ObjectWrap<SftpNode> {
public:
	std::atomic<bool> is_resolved = false;

	Napi::ThreadSafeFunction tsfn;
	std::binary_semaphore    sem;

	static Napi::Function init(Napi::Env env);
	SftpNode(const Napi::CallbackInfo& info);

	void         cleanup(Napi::Env env);
	SftpWatch_t* get_watch_ctx();
	EvtFile_t*   set_file_event(EventFile_t& ev, bool& status, DirItem_t* file);
	EvtFile_t*   get_file_event();
	void         delete_file_event();

	Napi::Value js_sync_dir(const Napi::CallbackInfo& info);
	Napi::Value js_connect(const Napi::CallbackInfo& info);
	Napi::Value js_sync_stop(const Napi::CallbackInfo& info);

private:
	SftpWatch_t* ctx = nullptr;
	EvtFile_t*   ev_file; /**< pointer to event data for js callback */

	Napi::Promise::Deferred deferred;
};

#endif
