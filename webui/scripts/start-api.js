const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const serverDir = path.join(__dirname, '..', 'server');

const candidates = [
  path.join(serverDir, 'venv', 'Scripts', 'python.exe'),
  path.join(serverDir, 'venv', 'Scripts', 'python'),
  path.join(serverDir, 'venv', 'bin', 'python'),
];

const pythonPath = candidates.find((candidate) => fs.existsSync(candidate));

if (!pythonPath) {
  console.error('Could not find a Python executable in ./server/venv.');
  console.error('Expected one of:');
  candidates.forEach((candidate) => console.error(`  - ${candidate}`));
  console.error('Create the venv first:');
  console.error('  cd webui/server && python -m venv venv');
  process.exit(1);
}

const child = spawn(pythonPath, ['server.py'], {
  cwd: serverDir,
  stdio: 'inherit',
});

child.on('exit', (code, signal) => {
  if (signal) {
    process.kill(process.pid, signal);
    return;
  }
  process.exit(code || 0);
});

child.on('error', (err) => {
  console.error('Failed to start API server:', err.message);
  process.exit(1);
});
