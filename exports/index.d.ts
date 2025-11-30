/**
 * @packageDocumentation
 * @document ../README.MD
 * @document ../CHANGELOG.MD
 * */

/** File Event name enum */
export type FileEvent = 
	/** New Remote File. Download event */
	'down' 

	/** New Local File. Upload Event */
	| 'up' 
	
	/** Remote file is deleted */
	| 'delR'
	
	/** Local file is deleted */
	| 'delL';

/** File type enum */
export type FileType = 
	/** Regular File */
	'f' 
	
	/** Directory */
	| 'd';

/** Synchronization Event names enum */
export type SyncEvent = 
	/** Data event. Triggered when one of {@link FileEvent} is found. */
	'data' 
	
	/** Error Event. Triggered when error happens during synchronization. */
	| 'error';

/** Error Type enum */
export enum ErrorType {
	/** Other Error outside of sync process. i.e. no auth method is provided */
	Custom = 0,

	/** Local Error. i.e. file not found on local directory */
	Local = 1,

	/** SFTP Session Error. i.e. Failed auth */
	Session = 2,

	/** SFTP File error. i.e. Permission denied on remote file */
	Sftp = 3
}

/**
 * SFTP configuration
 */
export interface Config {
	/** Target address in IP or domain name */
	host: string;

	/** Target port number
	 * @defaultValue 22
	 */
	port?: number;

	/** Remote root directory path */
	remotePath: string;

	/** Locale root directory path */
	localPath: string;

	/** Username for auth */
	username: string;

	/** Path to Private Key file */
	privkey?: string;

	/** Path to Public Key file. Must be defined if {@link Config.privkey} is not empty */
	pubkey?: string;

	/** Password for auth. Must be defined if {@link Config.privkey} is empty, otherwise will be ignored */
	password?: string;

	/** Use keyboard interactive auth to send the {@link Config.password}. 
	 * @defaultValue true
	*/
	useKeyboard?: boolean;

	/** Timeout in seconds. 
	 * @defaultValue 10
	*/
	timeout?: number;

	/** Delay between synchronizaion in milliseconds. 
	 * @defaultValue 1000
	*/
	delayMs?: number;

	/** Max error count before attempting to reconnect. 
	 * @defaultValue 3
	*/
	maxErrCount?: number;
}

/**
 * Synchronization File Info
 */
export interface FileInfo {
	/** Sync event type */
	evt: FileEvent;

	/** Sync status. true for finished, false for synchronization start */
	status: boolean;

	/** Type of synced file */
	type: FileType;

	/** File modification time in UNIX Timestamp milliseconds */
	time: number;

	/** File size in bytes */
	size: number;

	/** File name */
	name: string;

	/** File permissions in octal format */
	perm: number;
}

/**
 * Synchronization File Error
 */
export interface FileError {
	/** Error type */
	type: ErrorType;

	/** Error number */
	code: number;

	/** String representation of Error number */
	msg: string;

	/** Path when error happened, relative to root path */
	path: string;
}

/**
 * Callback for synchronization data
 * @param info - synced file data
 */
export type SyncCallback = (info: FileInfo) => void;

/**
 * Callback for synchronization error
 * @param error - error details
 */
export type ErrorCallback = (error: FileError) => void;

/**
 * SftpWatch instance.
 * 
 * This class is exported as the default. The usage should be like the following
 * 
 * ```ts
 * import SftpWatch from '@iqrok/sftp-watch';
 * const sftp = new SftpWatch({...});
 * ```
 */
export default class SftpWatch {
	/**
	 * Create an instance of SftpWatch
	 * @param config - configuration for SFTP operation
	 * @throws if invalid configuration is found
	 */
	constructor(config: Config);

	/**
	 * Connect to remote host
	 * @returns true if success, false if failed
	 */
	connect(): boolean;

	/**
	 * Register callback for synchronization process.
	 * Must be called before {@link SftpWatch.sync} is called.
	 * @param name - name of the event
	 * @param callback - callback function would be registered
	 * @returns instance for SftpWatch, for chained methods
	 * @throws if name is not {@link SyncEvent}
	 */
	on(name: SyncEvent, callback: SyncCallback | ErrorCallback): this;
	
	/**
	 * Start synchronization process. Must be called only after connected to remote host.
	 * @returns true if successfully started
	 * @throws if synchronization is already started
	 * @throws if not connected
	 */
	sync(): boolean;

	/**
	 * Stop synchronization process. Will resolve after synchronization process is stopped.
	 * Wait until the stop is finished if before sync process is re-started 
	 * @returns Promised resolve to instance id
	 * @throws if synchronization is not started
	 * @throws if stop is requested again before the provious stop is finished
	 */
	stop(): Promise<string>;

	/**
	 * Get the last error happened in synchronization process.
	 * The last error won't be reset when successfull operation is happened
	 * @returns last error
	 */
	getError(): FileError;
}
