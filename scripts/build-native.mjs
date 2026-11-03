import { execFileSync } from 'node:child_process';
import { cpSync, mkdirSync, rmSync } from 'node:fs';
import path from 'node:path';
import process from 'node:process';

const root = process.cwd();
const isWindows = process.platform === 'win32';
const vendor = path.join(root, 'vendor', 'onnxruntime');
const buildDir = path.join(root, 'build', 'native');
const distDir = path.join(root, 'dist', 'native');
const run = (cmd, args) => {
  console.log(`$ ${cmd} ${args.join(' ')}`);
  execFileSync(cmd, args, { stdio: 'inherit' });
};

run('node', ['scripts/fetch-native-deps.mjs']);
rmSync(buildDir, { recursive: true, force: true });
rmSync(distDir, { recursive: true, force: true });
mkdirSync(distDir, { recursive: true });
const cmakeArgs = ['-S', 'native', '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release', `-DONNXRUNTIME_DIR=${vendor}`];
cmakeArgs.push('-G', 'Ninja');
run('cmake', cmakeArgs);
run('cmake', ['--build', buildDir, '--config', 'Release', '--parallel']);

run('cmake', ['--install', buildDir, '--prefix', distDir]);
cpSync(path.join(root, 'models', 'u2netp.onnx'), path.join(distDir, 'u2netp.onnx'));
console.log(`Native camera ready: ${distDir}`);
