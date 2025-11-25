#include "sftp_node_api.hpp"
#include "sftp_err.hpp"

#include "debug.hpp"

void SftpNode::tsfn_sync_finalizer(
	Napi::Env env, SftpNode* data, SftpWatch_t* ctx)
{
	SftpNode* node_ctx = static_cast<SftpNode*>(data);

	SftpWatch::disconnect(ctx);
	node_ctx->cleanup(env);
}

void SftpNode::tsfn_sync_cb(
	Napi::Env env, Napi::Function js_cb, SftpNode* node_ctx)
{
	Napi::Object obj = Napi::Object::New(env);

	EvtFile_t* ev = node_ctx->get_file_event();

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
	node_ctx->delete_file_event();

	js_cb.Call({ obj });

	// release the lock
	node_ctx->sem_sync.release();
}

void SftpNode::tsfn_sync_js_call(SftpWatch_t* ctx, UserData_t data,
	DirItem_t* file, bool status, EventFile_t ev)
{
	(void)ctx;

	SftpNode* node_ctx = static_cast<SftpNode*>(data);

	// need to use heap, avoiding data lost when race condition occurs
	node_ctx->set_file_event(ev, status, file);

	// BlockingCall() should never fail, since max queue size is 0
	if (node_ctx->tsfn_sync.BlockingCall(node_ctx, tsfn_sync_cb) != napi_ok) {
		Napi::Error::Fatal("new file err", "BlockingCall() failed");
	}

	// wait until the BlockingCall is finished
	node_ctx->sem_sync.acquire();
}

void SftpNode::tsfn_err_cb(
	Napi::Env env, Napi::Function js_cb, SftpNode* node_ctx)
{
	SyncErr_t*             error = &node_ctx->ctx->last_error;
	Napi::ObjectReference* res   = node_ctx->create_obj_error(env, error);

	js_cb.Call({ res->Value() });

	//~ delete error;

	// release the lock
	node_ctx->sem_err.release();
}

void SftpNode::tsfn_err_js_call(
	SftpWatch_t* ctx, UserData_t data, SyncErr_t* error)
{
	(void)ctx;
	(void)error;

	SftpNode* node_ctx = static_cast<SftpNode*>(data);

	if (!node_ctx->tsfn_err) return;

	// BlockingCall() should never fail, since max queue size is 0
	if (node_ctx->tsfn_err.BlockingCall(node_ctx, tsfn_err_cb) != napi_ok) {
		Napi::Error::Fatal("new file err", "BlockingCall() failed");
	};

	// wait until the BlockingCall is finished
	node_ctx->sem_err.acquire();
}

void SftpNode::thread_cleanup(SftpWatch_t* ctx, UserData_t data)
{
	(void)ctx;
	SftpNode* node_ctx = static_cast<SftpNode*>(data);
	node_ctx->tsfn_sync.Release();
	if (node_ctx->tsfn_err) node_ctx->tsfn_err.Abort();
}

SftpNode::SftpNode(const Napi::CallbackInfo& info)
	: Napi::ObjectWrap<SftpNode>(info)
	, sem_sync(0) // semaphore is initially locked
	, sem_err(0)  // semaphore is initially locked
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
	SftpWatch_t* ctx = new SftpWatch_t(host, username, pubkey, privkey,
		password, remote_dir, local_dir, SftpNode::tsfn_sync_js_call,
		SftpNode::tsfn_err_js_call, SftpNode::thread_cleanup);

	SftpWatch::set_user_data(ctx, static_cast<UserData_t>(this));

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

	Napi::Object o_error = Napi::Object::New(env);

	obj_err = Napi::Persistent(o_error);
	obj_err.SuppressDestruct();

	env.AddCleanupHook([](void* data) {
		(void)data;
		LOG_DBG("CLEANING UP HOOK\n");
	}, this);

	this->ctx = ctx;
}

EvtFile_t* SftpNode::set_file_event(
	EventFile_t& ev, bool& status, DirItem_t* file)
{
	this->ev_file         = new EvtFile_t;
	this->ev_file->ev     = ev;
	this->ev_file->status = status;
	this->ev_file->file   = file;

	return this->ev_file;
}

EvtFile_t* SftpNode::get_file_event()
{
	return this->ev_file;
}

void SftpNode::delete_file_event()
{
	delete this->ev_file;
}

SftpWatch_t* SftpNode::get_watch_ctx()
{
	if (this->ctx) return this->ctx;
	return nullptr;
}

void SftpNode::cleanup(Napi::Env env)
{
	this->ctx->thread.join();

	this->deferred.Resolve(Napi::String::New(env, this->ctx->host));
	this->is_resolved = true;

	// FIXME: Restart after cleaning up
	SftpWatch::clear(this->ctx);
}

Napi::Value SftpNode::connect(const Napi::CallbackInfo& info)
{
	Napi::Env env         = info.Env();

	if (SftpWatch::connect_or_reconnect(this->ctx)) {
		return Napi::Boolean::New(env, false);
	}

	return Napi::Boolean::New(env, true);
}

Napi::Value SftpNode::sync_start(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (!this->tsfn_sync) {
		Napi::Error::Fatal("SyncDir", "Data Event callback is empty");
	}

	if (this->is_resolved) {
		this->deferred    = Napi::Promise::Deferred::New(env);
		this->is_resolved = false;
	}

	SftpWatch_t* ctx = this->ctx;
	SftpWatch::start(ctx);

	return this->deferred.Promise();
}

Napi::Value SftpNode::sync_stop(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	SftpWatch::request_stop(this->ctx);

	return env.Undefined();
}

Napi::Value SftpNode::listen_to(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	if (info.Length() < 2) {
		Napi::TypeError::New(env, "Invalid params. Should be <event, callback>")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[0].IsString()) {
		Napi::TypeError::New(env, "Event name must be a string")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	if (!info[1].IsFunction()) {
		Napi::TypeError::New(env, "Event Callback is not a Function")
			.ThrowAsJavaScriptException();
		return Napi::Boolean::New(env, false);
	}

	std::string name = info[0].As<Napi::String>().Utf8Value();
	std::string resource_name
		= name + "_" + ctx->host + ":" + std::to_string(ctx->port);

	SftpWatch_t* ctx = this->ctx;

	if (name == "data") {
		this->tsfn_sync = Napi::ThreadSafeFunction::New(env, // Environment
			info[1].As<Napi::Function>(),  // JS function from caller
			resource_name,                 // Resource name
			0,                             // Max queue_si size (0 = unlimited).
			1,                             // Initial thread count
			ctx,                           // Context,
			SftpNode::tsfn_sync_finalizer, // Finalizer
			this                           // Finalizer data
		);
	} else if (name == "error") {
		this->tsfn_err = Napi::ThreadSafeFunction::New(env, // Environment
			info[1].As<Napi::Function>(), // JS function from caller
			resource_name,                // Resource name
			0,                            // Max queue_si size (0 = unlimited).
			1,                            // Initial thread count
			ctx                           // Context,
		);
	} else {
		Napi::TypeError::New(env, "Unknown Event name " + name)
			.ThrowAsJavaScriptException();
	}

	return info.This();
}

Napi::ObjectReference* SftpNode::create_obj_error(
	Napi::Env env, SyncErr_t* error)
{
	this->obj_err.Set("type", Napi::Number::New(env, error->type));
	this->obj_err.Set("code", Napi::Number::New(env, error->code));

	if (error->path) {
		this->obj_err.Set("path", Napi::String::New(env, error->path));
	} else {
		this->obj_err.Set("path", Napi::String::New(env, ""));
	}

	if (error->msg) {
		std::string msg(error->msg);

		if (error->type == ERR_FROM_SESSION) {
			msg = msg + " [" + SftpErr::session_error(error->code) + "]";
		}

		this->obj_err.Set("msg", Napi::String::New(env, msg));
	} else {
		this->obj_err.Set("msg", Napi::String::New(env, "No Error"));
	}

	return &this->obj_err;
}

Napi::Value SftpNode::get_error(const Napi::CallbackInfo& info)
{
	Napi::Env env = info.Env();

	SyncErr_t*             error = &this->ctx->last_error;
	Napi::ObjectReference* res   = this->create_obj_error(env, error);

	return res->Value();
}

Napi::Object init_napi(Napi::Env env, Napi::Object exports)
{
	std::initializer_list<Napi::ClassPropertyDescriptor<SftpNode>> properties
		= {
			  SftpNode::InstanceMethod("connect", &SftpNode::connect),
			  SftpNode::InstanceMethod("sync", &SftpNode::sync_start),
			  SftpNode::InstanceMethod("stop", &SftpNode::sync_stop),
			  SftpNode::InstanceMethod("on", &SftpNode::listen_to),
			  SftpNode::InstanceMethod("getError", &SftpNode::get_error),
		  };

	Napi::Function func = SftpNode::DefineClass(env, "SftpNode", properties);

	Napi::FunctionReference constructor = Napi::Persistent(func);

	exports.Set("SftpWatch", func);

	return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, init_napi);
