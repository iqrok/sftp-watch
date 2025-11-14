const { SftpWatch } = require('..');

const config = require('./config.js');

// overwrite localPath to use sample/test directory
config.localPath = __dirname + '/test';

const fs = require('fs');
if (fs.existsSync(config.localPath)) {
	fs.rmSync(config.localPath, { recursive: true, });
}

fs.mkdirSync(config.localPath);

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
	case 'up': return '\x1b[95m';
	case 'down': return '\x1b[1m\x1b[32m';
	default   : return '\x1b[0m';
	}
}

try {
	const sftp = new SftpWatch(config);
	const connectId = sftp.connect();

	// should yield a number > 0
	if (!connectId) throw 'Failed to connect to SFTP Server';

	// save returned promise for stopping later
	const sync = sftp.sync((file) => {
			const dt  = new Date(file.time);
			const now = new Date();
			console.log(
				`${now.toLocaleString('Lt-lt')} => `
				+ `${file.status ? 'FINISHED' : 'STARTING'} `
				+ `${getEvtColor(file.evt)}[${file.evt.padEnd(4)}]\x1b[0m `
				+ `\x1b[3m<type ${file.type}>\x1b[0m `
				+ `\x1b[34m${dt.toLocaleString('Lt-lt')}\x1b[0m `
				+ `\x1b[34m${file.time}\x1b[0m `
				+ `${file.perm.toString(8)} `
				+ `${formatBytes(file.size)} `
				+ `\x1b[1m\x1b[${file.type == 'f' ? 33 : 36}m${file.name}\x1b[0m `
			);
		});

	process.on('SIGINT', async () => {
			console.log("\nSTOPPING ID", connectId);

			// request stop for connectId
			sftp.stop(connectId);

			// wait until sync process is stopped
			const stoppedId = await sync.then(id => id);

			console.log("\nSTOPPED", stoppedId);

			process.exit(0);
		});
} catch (error) {
	console.error(error);
}
