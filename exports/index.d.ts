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
}

export type SyncCallback = (info: FileInfo) => void;

declare const SftpWatch: {
	connect(): boolean;
	sync(syncCb: SyncCallback): Promise<string>;
	stop(): void;
};

export = SftpWatch;
