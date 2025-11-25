import fs from 'node:fs';
import SftpWatch from '@iqrok/sftp-watch';
import type { FileInfo } from '@iqrok/sftp-watch';

import config from './config.json' with { type: "json" };

// overwrite localPath to use sample/test directory
config.localPath = import.meta.dirname + '/test';

if (process.argv.length > 2) {
	if (fs.existsSync(config.localPath)) {
		console.log("Delete local path")
		fs.rmSync(config.localPath, { recursive: true, });
	}
}

if (!fs.existsSync(config.localPath)) fs.mkdirSync(config.localPath);

function formatBytes(size: number, useBinary: boolean = true): string {
	const units: string[] = useBinary
		? ['B', 'KiB', 'MiB', 'GiB', 'TiB']
		: ['B', 'KB', 'MB', 'GB', 'TB'];
	const step: number = useBinary ? 1024 : 1000;
	const pad: number = useBinary ? 3 : 2;

	if (size < 0 || !size) return '	0 ' + 'B'.padEnd(pad);

	let i: number = 0;
	for (i = 0; size >= step && i < units.length - 1; i++) size /= step;

	// right-align number, unit always occupies last 2–3 chars
	const formatted: string = size.toFixed(size < 10 && i > 0 ? 2 : 0);
	return `${formatted.padStart(6)} ${units[i].padEnd(pad)}`;
}

function getEvtColor(evt: string): string {
	switch(evt) {
	case 'delR': return '\x1b[1m\x1b[31m';
	case 'delL': return '\x1b[1m\x1b[91m';
	case 'up': return '\x1b[35m';
	case 'down': return '\x1b[1m\x1b[32m';
	default   : return '\x1b[0m';
	}
}

function syncCb(file: FileInfo): void {
	const dt: Date  = new Date(file.time);
	const now: Date = new Date();

	console.log(
		`${now.toLocaleString('Lt-lt')} => `
		+ `${file.status ? 'FINISHED' : 'STARTING'} `
		+ `${getEvtColor(file.evt)}[${file.evt.padEnd(4)}]\x1b[0m `
		+ `\x1b[3m<type ${file.type}>\x1b[0m `
		+ `\x1b[34m${dt.toLocaleString('Lt-lt')}\x1b[0m `
		+ `${file.perm.toString(8)} `
		+ `${formatBytes(file.size)} `
		+ `\x1b[1m\x1b[${file.type == 'f' ? 33 : 36}m${file.name}\x1b[0m `
	);
}

function errorCb(err) : void {
	console.error('Ini ERROR');
	console.log(err);
}

let i: number = 0;
const sftp = new SftpWatch(config);

async function stopProc() : void {
		console.log('\nSTOPPING', i);

		// request stop
		const stop = await sftp.stop();

		console.log("\nSTOPPED", stop);

		// restart once again
		if (i++ > 1) {
			process.exit(0);
		} else {
			sftp.connect();
			sftp.sync(syncCb);
			setTimeout(stopProc, 2500);
		}
	};

if (!sftp.connect()) {
	console.error('Failed to connect to SFTP server');
	process.exit(1);
}

sftp.on("data", syncCb)
	.on("error", errorCb)
	.sync();

process.on('SIGINT', stopProc);
//~ setTimeout(stopProc, 3000);
