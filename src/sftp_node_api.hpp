#ifndef _SFTP_NODE_API_HPP
#define _SFTP_NODE_API_HPP

#include <napi.h>

#include "sftp_node.hpp"

class SftpNode : public Napi::ObjectWrap<SftpNode> {
public:
	static Napi::Function init(Napi::Env env);
	SftpNode(const Napi::CallbackInfo& info);
	Napi::Value js_sync_dir(const Napi::CallbackInfo& info);
	Napi::Value js_connect(const Napi::CallbackInfo& info);
	Napi::Value js_sync_stop(const Napi::CallbackInfo& info);

	Napi::Promise::Deferred  deferred;
	Napi::ThreadSafeFunction tsfn;
	SftpWatch_t*             ctx = nullptr;
};

#endif
