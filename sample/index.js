const SftpWatch = require('..');
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
	case 'del': return '\x1b[1m\x1b[31m';
	case 'new': return '\x1b[32m';
	case 'mod': return '\x1b[1m\x1b[32m';
	default   : return '\x1b[0m';
	}
}

try {
	const connectId = SftpWatch.connect(config);

	// should be a number and > 0
	if (!connectId) throw 'Failed to connect to SFTP Server';

	process.on('SIGINT', async () => {
		await SftpWatch.stop(connectId);
		setTimeout(() => process.exit(0), 10_000);
	});

	SftpWatch.sync(connectId, (file) => {
			const dt  = new Date(file.time);
			const now = new Date();
			console.log(
				`${now.toLocaleString('Lt-lt')} => `
				+ `${getEvtColor(file.evt)}[${file.evt}]\x1b[0m `
				+ `\x1b[3m<type ${file.type}>\x1b[0m `
				+ `\x1b[34m${dt.toLocaleString('Lt-lt')}\x1b[0m `
				+ `${file.perm.toString(8)} `
				+ `${formatBytes(file.size)} `
				+ `\x1b[1m\x1b[${file.type == 'f' ? 33 : 36}m${file.name}\x1b[0m `
			);
		});
} catch (error) {
	console.error(error);
}
