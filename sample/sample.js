const SftpWatch = require('../build/Release/sftp-watch.node');
const config = require('./config.js');

try {
	const connect = SftpWatch.connect(config);

	if (!connect) throw 'Failed to connect to SFTP Server';

	SftpWatch.sync(connect, '/tmp/test', '/tmp', (list) => {
			if (list.created.length > 0) {
				console.log('New or Updated File(s)');
				console.table(list.created);
				console.log('*********************************************');
			}

			if (list.removed.length > 0) {
				console.log('Removed File(s)');
				console.table(list.removed);
				console.log('=============================================');
			}
		});
} catch (error) {
	console.error(error);
}
