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
vcpkg install libssh2 openssl zlib
```
NOTE: I am not very familiar with building on Windows. I installed the libraries system-wide to mimic the Linux experience.

You can read how I prepared my Windows environment in [this note](https://gitlab.com/-/snippets/4901502)

## Example

```js
const SftpWatch = require('@iqrok/sftp-watch');

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

const sftp = new SftpWatch(config);

const syncCb = (info) => {
  console.log(`[${info.evt}] ${info.type === 'd' ? 'DIR' : 'FILE'} ${info.name}`);
};

sftp.connect();
const sync = sftp.on(syncCb).sync();

// stop monitoring on ctrl + c;
process.on('SIGINT', async () => {
  await sftp.stop();
  process.exit(0);
});
```

# Limitations

Known limitations are as follows:

- Sub-direcory and all its contents are synchronized. But no depth limit has been implemented yet.

- Synchronization happens immediately when a new file is detected. As a consequence, if the remote file is still being written (e.g., during download), the client will start syncing that file.

- Renaming and moving a file/directory is handled as deletion -> creation. It would be a problem if the file size is large or the directory contains many files

- No prebuilt binary exists. So, you really need to install the [prequsites](#prerequisites) above.
