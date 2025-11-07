#ifndef _SFTP_NODE_API_HPP
#define _SFTP_NODE_API_HPP

Napi::Value js_connect(const Napi::CallbackInfo& info);
Napi::Value js_sync_dir(const Napi::CallbackInfo& info);
Napi::Value js_sync_stop(const Napi::CallbackInfo& info);

#endif
