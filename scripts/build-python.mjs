import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, renameSync, rmSync } from 'node:fs';
import path from 'node:path';
import process from 'node:process';

const root = process.cwd();
const venv = path.join(root, '.venv-build');
const isWindows = process.platform === 'win32';
const python = isWindows
  ? path.join(venv, 'Scripts', 'python.exe')
  : path.join(venv, 'bin', 'python');
const separator = isWindows ? ';' : ':';
const outputName = isWindows ? 'thanos-virtual-camera.exe' : 'thanos-virtual-camera';

const run = (cmd, args) => {
  console.log(`$ ${cmd} ${args.join(' ')}`);
  execFileSync(cmd, args, { stdio: 'inherit' });
};

const venvPython = isWindows ? 'python' : 'python3';
rmPrinted();
run(venvPython, ['-m', 'venv', venv]);
run(python, ['-m', 'pip', 'install', '--upgrade', 'pip']);
run(python, ['-m', 'pip', 'install', '-r', 'requirements.txt', 'pyinstaller']);

rmSync(path.join(root, 'dist', 'python'), { recursive: true, force: true });
mkdirSync(path.join(root, 'dist', 'python'), { recursive: true });
run(python, [
  '-m', 'PyInstaller',
  '--noconfirm',
  '--clean',
  '--onefile',
  '--name', 'thanos-virtual-camera',
  '--add-data', `models/selfie_segmenter.tflite${separator}models`,
  '--collect-all', 'mediapipe',
  '--collect-all', 'cv2',
  '--collect-all', 'pyvirtualcam',
  '--collect-all', 'flask',
  '--collect-all', 'werkzeug',
  'camera_virtual.py',
]);

const produced = path.join(root, 'dist', outputName);
const target = path.join(root, 'dist', 'python', outputName);
if (!existsSync(produced)) {
  throw new Error(`PyInstaller output not found: ${produced}`);
}
renameSync(produced, target);
console.log(`Python backend ready: ${target}`);

function rmPrinted() {
  rmSync(venv, { recursive: true, force: true });
}
