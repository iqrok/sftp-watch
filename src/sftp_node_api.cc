#include "sftp_node_api.hpp"

static void sync_dir_finalizer(Napi::Env env, void* data, SftpWatch_t* ctx)
{
	SftpNode* node_ctx = static_cast<SftpNode*>(data);

	SyncDir::disconnect(ctx);

	ctx->thread.join();
	node_ctx->deferred.Resolve(Napi::Number::New(env, ctx->id));

	//~ watchers.erase(ctx->id);

	//~ if (watchers.empty()) SftpRemote::shutdown();

	delete ctx;
	node_ctx->ctx = nullptr;
}

static void sync_dir_tsfn_cb(
	Napi::Env env, Napi::Function js_cb, SftpWatch_t* ctx)
{
	Napi::Object obj = Napi::Object::New(env);

	EvtFile_t* ev = ctx->ev_file;

	switch (ev->ev) {

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

	default: {
	} break;
	}

	obj.Set("status", Napi::Boolean::New(env, ev->status));
	obj.Set("name", Napi::String::New(env, ev->file->name));
	obj.Set("type", Napi::String::New(env, SNOD_CHR2STR(ev->file->type)));
	obj.Set("size", Napi::Number::New(env, ev->file->attrs.filesize));
	obj.Set("time", Napi::Number::New(env, SNOD_SEC2MS(ev->file->attrs.mtime)));
	obj.Set("perm", Napi::Number::New(env, SNOD_FILE_PERM(ev->file->attrs)));

	// don't forget to delete the data, since we used dynamic allocation
	delete ev;

	js_cb.Call({ obj });

	// release the lock
	ctx->sem.release();
}

static void sync_dir_js_call(
	SftpWatch_t* ctx, void* data, DirItem_t* file, bool status, uint8_t ev)
{
	SftpNode* node_ctx = static_cast<SftpNode*>(data);

	// need to use heap, avoiding data lost when race condition occurs
	ctx->ev_file         = new EvtFile_t;
	ctx->ev_file->ev     = ev;
	ctx->ev_file->status = status;
	ctx->ev_file->file   = file;

	// BlockingCall() should never fail, since max queue size is 0
	if (node_ctx->tsfn.BlockingCall(ctx, sync_dir_tsfn_cb) != napi_ok) {
		Napi::Error::Fatal("new file err", "BlockingCall() failed");
	}

	// wait until the BlockingCall is finished
	ctx->sem.acquire();
}

static void thread_cleanup(SftpWatch_t* ctx, void* data)
{
	(void)ctx;
	SftpNode* node_ctx = static_cast<SftpNode*>(data);
	node_ctx->tsfn.Release();
}

SftpNode::SftpNode(const Napi::CallbackInfo& info)
	: Napi::ObjectWrap<SftpNode>(info)
	, deferred(Napi::Promise::Deferred::New(info.Env()))
{
	Napi::Env env = info.Env();

	if (info.Length() < 1 || !info[0].IsObject()) {
		Napi::TypeError::New(env, "Expected an object")
			.ThrowAsJavaScriptException();
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
		}

		host = arg.Get("host").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError::New(env, "'host' is undefined")
			.ThrowAsJavaScriptException();
	}

	if (arg.Has("username")) {
		if (!arg.Get("username").IsString() || arg.Get("username").IsEmpty()) {
			Napi::TypeError::New(env, "'username' is empty")
				.ThrowAsJavaScriptException();
		}

		username = arg.Get("username").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError::New(env, "'username' is undefined")
			.ThrowAsJavaScriptException();
	}

	if (arg.Has("remotePath")) {
		if (!arg.Get("remotePath").IsString()
			|| arg.Get("remotePath").IsEmpty()) {
			Napi::TypeError::New(env, "'remotePath' is empty")
				.ThrowAsJavaScriptException();
		}

		remote_dir.path = arg.Get("remotePath").As<Napi::String>().Utf8Value();
	} else {
		Napi::TypeError::New(env, "'remotePath' is undefined")
			.ThrowAsJavaScriptException();
	}

	if (arg.Has("localPath")) {
		if (!arg.Get("localPath").IsString()
			|| arg.Get("localPath").IsEmpty()) {
			Napi::TypeError::New(env, "'localPath' is empty")
				.ThrowAsJavaScriptException();
		}

		local_dir.path = arg.Get("localPath").As<Napi::String>().Utf8Value();
	}

	// ------------------------ Auth properties --------------------------------
	if (arg.Has("pubkey")) {
		if (!arg.Get("pubkey").IsString() || arg.Get("pubkey").IsEmpty()) {
			Napi::TypeError::New(env, "'pubkey' is empty")
				.ThrowAsJavaScriptException();
		}

		pubkey = arg.Get("pubkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("privkey")) {
		if (!arg.Get("privkey").IsString() || arg.Get("privkey").IsEmpty()) {
			Napi::TypeError::New(env, "'privkey' is empty")
				.ThrowAsJavaScriptException();
		}

		privkey = arg.Get("privkey").As<Napi::String>().Utf8Value();
	}

	if (arg.Has("password")) {
		if (!arg.Get("password").IsString() || arg.Get("password").IsEmpty()) {
			Napi::TypeError::New(env, "'password' is empty")
				.ThrowAsJavaScriptException();
		}

		password = arg.Get("password").As<Napi::String>().Utf8Value();
	}

	if ((pubkey.empty() || privkey.empty()) && password.empty()) {
		Napi::TypeError::New(env, "invalid auth").ThrowAsJavaScriptException();
	}

	// ------------------------ Init context -----------------------------------
	SftpWatch_t* ctx = new SftpWatch_t(0, host, username, pubkey, privkey,
		password, remote_dir, local_dir, sync_dir_js_call, thread_cleanup,
		(void*)this);

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

	//~ watchers[ctx->id] = ctx;
	this->ctx = ctx;
}

Napi::Value SftpNode::js_connect(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	for (uint8_t i = 0; i < 5; i++) {
		if (!SyncDir::connect_or_reconnect(this->ctx)) {
			return Napi::Boolean::New(env, true);
		}
	}

	return Napi::Boolean::New(env, false);
}

Napi::Value SftpNode::js_sync_dir(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 1) {
		Napi::TypeError::New(env, "Invalid params. Should be <callback>")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[0].IsFunction()) {
		Napi::TypeError::New(env, "Invalid Callback Function")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	//~ const uint32_t id    = info[0].As<Napi::Number>().Uint32Value();
	Napi::Function js_cb = info[0].As<Napi::Function>();

	//~ SftpWatch_t* ctx = watchers.at(id);
	SftpWatch_t* ctx = this->ctx;

	ctx->is_stopped = false;
	this->tsfn      = Napi::ThreadSafeFunction::New(env, // Environment
			 js_cb, // JS function from caller
			 std::string("watcher_") + std::to_string(9000), // Resource name
			 0,                  // Max queue_si size (0 = unlimited).
			 1,                  // Initial thread count
			 ctx,                // Context,
			 sync_dir_finalizer, // Finalizer
			 this                // Finalizer data
		 );

	ctx->thread = std::thread(SyncDir::sync_dir_thread, ctx);

	return this->deferred.Promise();
}

Napi::Value SftpNode::js_sync_stop(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	//~ const uint32_t id = info[0].As<Napi::Number>().Uint32Value();

	//~ if (!watchers.contains(id)) return Napi::Boolean::New(env, true);

	//~ SftpWatch_t* ctx = watchers.at(id);
	SftpWatch_t* ctx = this->ctx;
	ctx->is_stopped  = true;

	return env.Undefined();
}

Napi::Function SftpNode::init(Napi::Env env)
{
	return SftpNode::DefineClass(env, "SftpNode",
		{
			SftpNode::InstanceMethod("connect", &SftpNode::js_connect),
			SftpNode::InstanceMethod("sync", &SftpNode::js_sync_dir),
			SftpNode::InstanceMethod("stop", &SftpNode::js_sync_stop),
		});
}

Napi::Object init_napi(Napi::Env env, Napi::Object exports)
{
	Napi::String name = Napi::String::New(env, "SftpWatch");
	exports.Set(name, SftpNode::init(env));
	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, init_napi);

//~ Napi::Object init_napi(Napi::Env env, Napi::Object exports)
//~ {
//~ exports.Set(Napi::String::New(env, "connect"),
//~ Napi::Function::New(env, js_connect));
//~ exports.Set(
//~ Napi::String::New(env, "sync"), Napi::Function::New(env, js_sync_dir));
//~ exports.Set(
//~ Napi::String::New(env, "stop"), Napi::Function::New(env, js_sync_stop));

//~ return exports;
//~ }

//~ NODE_API_MODULE(NODE_GYP_MODULE_NAME, init_napi);
