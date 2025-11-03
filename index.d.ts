export type FileEvent = 'del' | 'new' | 'mod';
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
  timeout?: number;
  delayMs?: number;
  maxErrCount?: number;
}

export interface FileInfo {
  evt: FileEvent;
  type: FileType;
  time: number;
  size: number;
  name: string;
}

export type SyncCallback = (info: FileInfo) => void;

export function connect(config: Config): number;
export function sync(conId: number, syncCb: SyncCallback): true;
export function stop(conId: number): void;

declare const _default: {
  connect: typeof connect;
  sync: typeof sync;
  stop: typeof stop;
};

export default _default;
