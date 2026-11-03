const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('vcam', {
  request: (method, path, body) => ipcRenderer.invoke('api-request', method, path, body),
  backendStatus: () => ipcRenderer.invoke('backend-status'),
  openExternal: (url) => ipcRenderer.invoke('open-external', url),
  status: () => ipcRenderer.invoke('api-request', 'GET', '/status'),
  setMode: (mode) => ipcRenderer.invoke('api-request', 'POST', '/mode', { mode }),
  captureBackground: () => ipcRenderer.invoke('api-request', 'POST', '/bg'),
  resetBackground: () => ipcRenderer.invoke('api-request', 'POST', '/resetbg'),
  setThreshold: (value) => ipcRenderer.invoke('api-request', 'POST', '/threshold', { value }),
  frame: () => ipcRenderer.invoke('api-request', 'GET', '/frame.jpg'),
});
