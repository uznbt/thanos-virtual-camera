import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync } from 'node:fs';
import path from 'node:path';
import process from 'node:process';

const root = process.cwd();
const vendor = path.join(root, 'vendor', 'onnxruntime');
const isWindows = process.platform === 'win32';
const version = '1.31.1';
const asset = isWindows
  ? { file: `onnxruntime-win-x64-${version}.zip`, url: `https://github.com/microsoft/onnxruntime/releases/download/v${version}/onnxruntime-win-x64-${version}.zip`, sha256: '5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c' }
  : { file: `onnxruntime-linux-x64-${version}.tgz`, url: `https://github.com/microsoft/onnxruntime/releases/download/v${version}/onnxruntime-linux-x64-${version}.tgz`, sha256: 'c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9' };
const tempDir = path.join(root, '.native-deps');
const archive = path.join(tempDir, asset.file);
const marker = path.join(vendor, 'VERSION_NUMBER');

if (existsSync(marker) && readFileSync(marker, 'utf8').trim() === version) {
  console.log(`ONNX Runtime ${version} already available`);
  process.exit(0);
}

rmSync(tempDir, { recursive: true, force: true });
rmSync(vendor, { recursive: true, force: true });
mkdirSync(tempDir, { recursive: true });
mkdirSync(vendor, { recursive: true });
execFileSync('curl', ['-L', '--fail', '--retry', '3', '-o', archive, asset.url], { stdio: 'inherit' });
const hash = crypto.createHash('sha256').update(readFileSync(archive)).digest('hex');
if (hash !== asset.sha256) {
  rmSync(tempDir, { recursive: true, force: true });
  throw new Error(`ONNX Runtime checksum mismatch: got ${hash}, expected ${asset.sha256}`);
}
execFileSync('tar', isWindows ? ['-xf', archive, '-C', tempDir] : ['-xzf', archive, '-C', tempDir], { stdio: 'inherit' });
mkdirSync(vendor, { recursive: true });
execFileSync('tar', isWindows ? ['-xf', archive, '-C', vendor, '--strip-components=1'] : ['-xzf', archive, '-C', vendor, '--strip-components=1'], { stdio: 'inherit' });
rmSync(tempDir, { recursive: true, force: true });
console.log(`ONNX Runtime ${version} fetched to ${vendor}`);
