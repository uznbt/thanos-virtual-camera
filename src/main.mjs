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
      executable: path.join(process.resourcesPath, 'native', isWindows ? 'native-camera.exe' : 'native-camera'),
      model: path.join(process.resourcesPath, 'native', 'u2netp.onnx'),
    };
  }
  return {
    executable: isWindows
      ? path.join(root, 'build', 'native', 'Release', 'native-camera.exe')
      : path.join(root, 'build', 'native', 'native-camera'),
    model: path.join(root, 'models', 'u2netp.onnx'),
  };
}

function backendExecutable() {
  return backendCommand().executable;
}

function backendArgs() {
  const dst = process.env.THANOS_CAMERA_DST || (isWindows ? 'OBS Virtual Camera' : '/dev/video10');
  return ['--dst', dst, '--no-preview'];
}

function ensureLinuxLoopback() {
  if (!isLinux || fs.existsSync('/dev/video10')) return;
  execFile('pkexec', ['modprobe', 'v4l2loopback', 'devices=1', 'video_nr=10', 'card_label=VirtualCam', 'exclusive_caps=1'], () => {});
}

function startBackend() {
  if (backend && !backend.killed) return backend;
  ensureLinuxLoopback();
  const command = backendCommand();
  const executable = command.executable;
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
    env: { ...process.env, THANOS_CAMERA_HOME: app.getPath('userData'), THANOS_MODEL: command.model },
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
