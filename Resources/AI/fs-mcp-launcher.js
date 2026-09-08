const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

// __dirname is always .../Resources/AI — two levels up is the Modularity root
// Works from both the source tree and the built/portable install
const modularityRoot = path.resolve(__dirname, '..', '..');
const homeDir = process.env.HOME || process.env.USERPROFILE || '';

const candidates = [
  modularityRoot,
  path.join(modularityRoot, 'docs'),
  path.join(modularityRoot, 'Scripts'),
  path.join(modularityRoot, 'include'),
  path.join(modularityRoot, 'Resources'),
  homeDir ? path.join(homeDir, 'ModularityProjects') : '',
  process.env.MODULARITY_PROJECT_ROOT || ''
].filter(Boolean);
const existingPaths = candidates.filter(p => fs.existsSync(p));

const npx = process.platform === 'win32' ? 'npx.cmd' : 'npx';

const proc = spawn(
  npx,
  ['-y', '@modelcontextprotocol/server-filesystem', modularityRoot, ...existingPaths],
  { stdio: 'inherit' }
);

proc.on('exit', code => process.exit(code ?? 0));
