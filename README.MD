# SFTP Watch

A minimal client that connects to a remote SFTP target and continuously syncs a directory to a local path. A callback is triggered each time an item is synced (new, modified, or deleted).

Having some fun with [Node-API](https://nodejs.org/api/n-api.html) and [node-addon-api](https://github.com/nodejs/node-addon-api) to make a SFTP client in nodejs. [libssh2](https://libssh2.org/) is used to handle the SFTP Client operations.

# Prerequisites

Building this library requires:

1. C++ compiler
2. cmake (since I use cmake-js)
3. zlib
4. OpenSSL
5. libssh2

## Debian / Ubuntu

```bash
sudo apt install build-essential cmake libssl-dev zlib1g-dev libssh2-1-dev
```

## macOS

A C++ compiler is installed with Xcode.

use [Homebrew](https://brew.sh) to install the remaining dependencies:

```shell
brew install cmake openssl zlib libssh2
```

## Windows

[Download and install Visual Studio](https://visualstudio.microsoft.com/downloads/) to obtain a C++ compiler.

You can install CMake using either **winget** or **chocolatey**. These commands must be run from an Administrator shell.

```shell
choco install cmake

# or

winget install Kitware.CMake
```

it must be run from Administrator Shell.

For the libraries, use **vcpkg**:

```shell
vcpkg install openssl libssh2 zlib
```
NOTE: I am not very familiar with building on Windows. I installed the libraries system-wide to mimic the Linux experience.

You can read how I prepared my Windows environment in [this note](https://gitlab.com/-/snippets/4901502)

# Docs

## Interface

### Config

Configuration object for `sftp-watch` used to establish a connection and manage sync behavior.

| Key | Type | Mandatory | Description |
|-----|------|------------|-------------|
| **host** | `string` | Yes | Remote target's IP address or domain name. |
| **port** | `number` | No *(default = 22)* | Remote target's SFTP port (default is usually 22). |
| **remotePath** | `string` | Yes | Absolute path of the remote root directory to be synced. |
| **localPath** | `string` | Yes | Local directory where synced files are stored. |
| **username** | `string` | Yes | Username for authentication. |
| **password** | `string` | Yes *(if `privkey` is not provided)* | Password for authentication. Ignored when key-based authentication is used. |
| **privkey** | `string` | Yes *(if `password` is not provided)* | Path to private key for key-based authentication. |
| **pubkey** | `string` | Yes *(if `privkey` is provided)* | Path to public key for key-based authentication. |
| **use_keyboard** | `bool` | No *(default = true)* | Use keyboard interactive to send password. |
| **timeout** | `number` | No *(default = 60 seconds)* | Connection timeout in seconds. |
| **delayMs** | `number` | No *(default = 1000 ms)* | Delay between checks in milliseconds. |
| **maxErrCount** | `number` | No *(default = 3)* | Number of consecutive errors before restarting the connection. |

> **WARNING:** Both `remotePath` and `localPath` must exist.
> **Note:** Use either password-based **or** key-based authentication. If both are supplied, key-based authentication takes precedence.

---

### FileInfo

Information about each file or directory synced, passed to the callback.

| Key | Type | Description |
|-----|------|-------------|
| **evt** | `'del' \| 'new' \| 'mod'` | Event type - whether the file is deleted, newly created, or modified. |
| **type** | `'f' \| 'd'` | Type of item - `'f'` for file, `'d'` for directory. |
| **time** | `number` | UNIX timestamp of the last modification time. |
| **size** | `number` | File size in bytes. |
| **name** | `string` | File name without the root path. |

---

## Callback

### `syncCb(info)`

This callback is called every time a file or directory is successfully synced.

#### Parameters
- **info** - [`FileInfo`](#fileinfo) object containing details about the synced item.

---

## API

### `connect(config)`

Establishes a connection to the remote SFTP target.

#### Parameters
- **config** - [`Config`](#config) object.

#### Returns
- A **connection ID** if the connection is successfully established.
- Throws an **Error** if the configuration is invalid or the connection cannot be made.

---

### `sync(conId, syncCb)`

Starts the synchronization process using an active connection.

#### Parameters
- **conId** - `number`, the connection ID returned by [`connect`](#connectconfig).
- **syncCb** - `Function`, the callback function to handle file sync events.

#### Returns
- `true` if the sync process is successfully started.
- Throws an **Error** if parameters are invalid.

---

### `stop(conId)`

Stops the synchronization process for an active connection.

#### Parameters
- **conId** - `number`, the connection ID associated with the active sync process.

#### Returns
- `undefined`.
- Throws an **Error** if parameters are invalid.

---

## Example Usage

```js
const SftpWatch = require('..');
const { connect, sync, stop } = SftpWatch;

const config = {
  host: '192.168.1.10',
  port: 22,
  remotePath: '/var/data/incoming',
  localPath: './mirror',
  username: 'deploy',
  privkey: '/path/.ssh/id_rsa',
  pubkey: '/path/.ssh/id_rsa.pub',
  delayMs: 2000,
  maxErrCount: 5
};

const conId = connect(config);

const syncCb = (info) => {
  console.log(`[${info.evt}] ${info.type === 'd' ? 'DIR' : 'FILE'} ${info.name}`);
};

sync(conId, syncCb);

// stop(conId); // Stop if needed:
```

# Limitations

I created this module out of curiosity, so several limitations were intentionally made to keep my sanity. Known limitations are as follows:

- Only one-direction synchronization is supported, from remote to local.

- ~~Only single-level synchronization is implemented. Files inside subdirectories are not synchronized.~~ Sub-direcory and all its contents are synchronized. But no depth limit has been implemented yet.

- Synchronization happens immediately when a new file is detected. As a consequence, if the remote file is still being written (e.g., during download), the client will start syncing that file. Although the final result remains identical between remote and local, this may cause confusion since the 'modified' event keeps triggering until the remote writing process is finished.

- Renaming and moving a file/directory is handled as deletion -> creation. It would be a problem if the file size is large or the directory contains many files

- Error handling is still limited, particularly regarding failed connections and failed directory or file reads.

- Many errors are still hidden within the C++ layer and have not yet been exposed to JavaScript.

- No local file check is performed before synchronization. If the local directory is not empty, untracked files may exist.

- No prebuilt binary exists. So, you really need to install the [prequsites](#prerequisites) above.
