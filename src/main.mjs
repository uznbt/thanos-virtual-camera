import { app, BrowserWindow, ipcMain, shell } from 'electron';
import { spawn, execFile } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const root = path.join(__dirname, '..');
const isWindows = process.platform === 'win32';
const isLinux = process.platform === 'linux';
const apiBase = 'http://127.0.0.1:8765';

let backend = null;
let windowRef = null;

function backendCommand() {
  if (app.isPackaged) {
    return {
      executable: path.join(process.resourcesPath, 'python', isWindows ? 'thanos-virtual-camera.exe' : 'thanos-virtual-camera'),
      args: [],
    };
  }
  const localVenv = isWindows
    ? path.join(root, '.venv', 'Scripts', 'python.exe')
    : path.join(root, '.venv', 'bin', 'python');
  const buildVenv = isWindows
    ? path.join(root, '.venv-build', 'Scripts', 'python.exe')
    : path.join(root, '.venv-build', 'bin', 'python');
  return {
    executable: fs.existsSync(localVenv) ? localVenv : buildVenv,
    args: ['camera_virtual.py'],
  };
}

function backendExecutable() {
  return backendCommand().executable;
}

function backendArgs() {
  const dst = process.env.THANOS_CAMERA_DST || (isWindows ? 'OBS Virtual Camera' : '/dev/video10');
  const common = ['--no-preview', '--dst', dst];
  return [...backendCommand().args, ...common];
}

function ensureLinuxLoopback() {
  if (!isLinux || fs.existsSync('/dev/video10')) return;
  execFile('pkexec', ['modprobe', 'v4l2loopback', 'devices=1', 'video_nr=10', 'card_label=VirtualCam', 'exclusive_caps=1'], () => {});
}

function startBackend() {
  if (backend && !backend.killed) return backend;
  ensureLinuxLoopback();
  const executable = backendExecutable();
  const logDir = path.join(app.getPath('userData'), 'logs');
  fs.mkdirSync(logDir, { recursive: true });
  const logPath = path.join(logDir, 'backend.log');
  if (!fs.existsSync(executable)) {
    fs.writeFileSync(logPath, `Backend not found: ${executable}\n`);
    return null;
  }
  const out = fs.openSync(logPath, 'a');
  backend = spawn(executable, backendArgs(), {
    cwd: root,
    env: { ...process.env, THANOS_CAMERA_HOME: app.getPath('userData') },
    stdio: ['ignore', out, out],
    windowsHide: true,
  });
  backend.on('exit', (code, signal) => {
    fs.appendFileSync(logPath, `backend exited code=${code} signal=${signal}\n`);
    backend = null;
  });
  return backend;
}

function stopBackend() {
  if (backend && !backend.killed) backend.kill(isWindows ? undefined : 'SIGTERM');
  backend = null;
}

async function apiRequest(_, method, urlPath, body) {
  const response = await fetch(`${apiBase}${urlPath}`, {
    method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const contentType = response.headers.get('content-type') || '';
  if (contentType.includes('application/json')) {
    return { ok: response.ok, status: response.status, data: await response.json() };
  }
  const buffer = Buffer.from(await response.arrayBuffer());
  return { ok: response.ok, status: response.status, bytes: buffer.toString('base64'), contentType };
}

function createWindow() {
  windowRef = new BrowserWindow({
    width: 1120,
    height: 760,
    minWidth: 920,
    minHeight: 620,
    title: 'Thanos Virtual Camera',
    icon: path.join(root, 'assets', 'icon.png'),
    backgroundColor: '#09060f',
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  });
  windowRef.loadFile(path.join(__dirname, 'index.html'));
}

ipcMain.handle('api-request', apiRequest);
ipcMain.handle('backend-status', () => ({
  running: Boolean(backend && !backend.killed),
  platform: process.platform,
  packaged: app.isPackaged,
  executable: backendExecutable(),
}));
ipcMain.handle('open-external', (_, url) => shell.openExternal(url));

app.whenReady().then(() => {
  startBackend();
  createWindow();
});
app.on('window-all-closed', () => app.quit());
app.on('before-quit', stopBackend);
