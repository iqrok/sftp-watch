const { SftpWatch } = require('..');

const config = require('./config.js');

// overwrite localPath to use sample/test directory
config.localPath = __dirname + '/test';

const fs = require('fs');
if (process.argv.length > 2) {
	if (fs.existsSync(config.localPath)) {
		console.log("Delete local path")
		fs.rmSync(config.localPath, { recursive: true, });
	}
}

if (!fs.existsSync(config.localPath)) fs.mkdirSync(config.localPath);

function formatBytes(size, useBinary = true) {
	const units = useBinary
		? ['B', 'KiB', 'MiB', 'GiB', 'TiB']
		: ['B', 'KB', 'MB', 'GB', 'TB'];
	const step = useBinary ? 1024 : 1000;
	const pad = useBinary ? 3 : 2;

	if (size < 0 || !size) return '	0 ' + 'B'.padEnd(pad);

	let i = 0;
	for (i = 0; size >= step && i < units.length - 1; i++) size /= step;

	// right-align number, unit always occupies last 2–3 chars
	const formatted = size.toFixed(size < 10 && i > 0 ? 2 : 0);
	return `${formatted.padStart(6)} ${units[i].padEnd(pad)}`;
}

function getEvtColor(evt) {
	switch(evt) {
	case 'delR': return '\x1b[1m\x1b[31m';
	case 'delL': return '\x1b[1m\x1b[91m';
	case 'up': return '\x1b[35m';
	case 'down': return '\x1b[1m\x1b[32m';
	default   : return '\x1b[0m';
	}
}

function syncCb(file) {
	const dt  = new Date(file.time);
	const now = new Date();

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

let i = 0;
const sftp = new SftpWatch(config);

if (!sftp.connect()) {
	console.error('Failed to connect to SFTP server');
	process.exit(1);
}

// save returned promise for stopping later
let sync = sftp.sync(syncCb);

process.on('SIGINT', async () => {
		console.log('\nSTOPPING');

		// request stop
		sftp.stop();

		// wait until sync process is stopped
		const stop = await sync.then(id => id);

		console.log("\nSTOPPED", stop);

		// restart once again
		if (i++ > 0) {
			process.exit(0);
		} else {
			sftp.connect();
			sync = sftp.sync(syncCb);
		}
	});
