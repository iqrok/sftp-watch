const SftpWatch = require('../build/Release/sftp-watch.node');
const config = require('./config.js');

try {
	const connect = SftpWatch.connect(config);
	if (connect) {
		SftpWatch.sync(connect, '/tmp', '/tmp', (list) => {
				console.table(list);
			});
	}
} catch (error) {
	console.log(error);
}
//~ const connect = SftpWatch.connect(20);
