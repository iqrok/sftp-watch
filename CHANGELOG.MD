# CHAGELOG

## 0.5.0
- Expose SFTP error to javascript via callback function
- Added `on()` to register callbacks
- Added `getError()` to get last error details
- Moved promise to `stop()`. 
- Fixed restarting synchronization process.
- Fixed reconnecting on lost connection.

## 0.4.0

- change into a class. The lifetime of the object is managed by Node API
- Fixed memory leak on disconnection
- Fixed conflict resolution when base is still empty

## 0.3.0

- 2-way synchronization between remote and local
- Synchronize subdirectories. No check on subdirectory max depth

## 0.2.0

- Password authentication with Keyboard Interactive
- Set File attribute to mimic remote file, on POSIX platform

## 0.1.0

- Basic synchronization from remote to local
