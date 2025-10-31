const SftpWatch = require('../build/Release/sftp-watch.node');
const config = require('./config.js');

try {
	const connect = SftpWatch.connect(config);
	if (connect) {
		SftpWatch.sync(connect, '/tmp/test', '/tmp', (list) => {
				console.table(list.created);
				console.table(list.removed);
				console.log('================================================');
			});
	}
} catch (error) {
	console.log(error);
}
//~ const connect = SftpWatch.connect(20);
