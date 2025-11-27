export type FileEvent = 'down' | 'up' | 'delR'| 'delL';
export type FileType = 'f' | 'd';

export interface Config {
	host: string;
	port?: number;
	remotePath: string;
	localPath: string;
	username: string;
	password?: string;
	privkey?: string;
	pubkey?: string;
	useKeyboard?: boolean;
	timeout?: number;
	delayMs?: number;
	maxErrCount?: number;
}

export interface FileInfo {
	evt: FileEvent;
	status: boolean;
	type: FileType;
	time: number;
	size: number;
	name: string;
	perm: number;
}

export interface FileError {
	type: number;
	code: number;
	msg: string;
	path: string;
}

export type SyncCallback = (info: FileInfo) => void;
export type ErrorCallback = (error: FileError) => void;

export default class SftpWatch {
	constructor(config: Config);
	connect(): boolean;
	sync(): boolean;
	on(name: string, callback: SyncCallback | ErrorCallback): this;
	stop(): Promise<string>;
	getError(): FileError;
}
