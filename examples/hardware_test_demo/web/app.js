/* 统一显示后端响应，确保原生错误码和原生输出能在网页中复核。 */
function showResult(value) {
  document.getElementById('result-output').textContent = JSON.stringify(value, null, 2);
}

/* 调用本 Demo 的 JSON API；密码只在提交时发送，不写入页面结果。 */
async function api(path, options = {}) {
  const response = await fetch(path, {
    cache: 'no-store',
    headers: { 'Content-Type': 'application/json' },
    ...options
  });
  const value = await response.json();
  if (!response.ok || value.ok === false) {
    throw value;
  }
  return value;
}

/* 切换侧边栏页面；音频页面首次打开时才加载独立音频服务。 */
function switchPage(pageId) {
  document.querySelectorAll('.nav-item').forEach((item) => {
    item.classList.toggle('active', item.dataset.page === pageId);
  });
  document.querySelectorAll('.page').forEach((page) => {
    page.classList.toggle('active-page', page.id === pageId);
  });
  if (pageId === 'camera-page') {
    startCameraPreview();
    refreshCameraStatus();
  } else {
    stopCameraPreview();
  }
}

/* 摄像头页面直接调用本 Demo 的同源接口，避免依赖第二个服务进程。 */
let cameraPreviewTimer = null;
let cameraPreviewBusy = false;

/* 读取摄像头实际状态，页面显示固定的 640x480@5 FPS 配置。 */
async function refreshCameraStatus() {
  try {
    const value = await api('/api/v1/camera/status');
    document.getElementById('camera-status').textContent = JSON.stringify(value, null, 2);
  } catch (error) {
    document.getElementById('camera-status').textContent = JSON.stringify(error, null, 2);
  }
}

/* 每 200 ms 获取一张 JPEG，显示刷新频率与摄像头 5 FPS 输出一致。 */
async function refreshCameraFrame() {
  if (cameraPreviewBusy) return;
  cameraPreviewBusy = true;
  try {
    const response = await fetch(`/api/v1/camera/snapshot.jpg?ts=${Date.now()}`, {
      cache: 'no-store'
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = await response.blob();
    const image = document.getElementById('camera-preview');
    const oldUrl = image.dataset.objectUrl;
    const newUrl = URL.createObjectURL(blob);
    image.onload = () => { if (oldUrl) URL.revokeObjectURL(oldUrl); };
    image.src = newUrl;
    image.dataset.objectUrl = newUrl;
    document.getElementById('camera-message').textContent = '摄像头输出正常：640×480，5 FPS';
  } catch (error) {
    document.getElementById('camera-message').textContent = `摄像头图像获取失败：${error.message}`;
  } finally {
    cameraPreviewBusy = false;
  }
}

function startCameraPreview() {
  if (cameraPreviewTimer) return;
  refreshCameraFrame();
  cameraPreviewTimer = setInterval(refreshCameraFrame, 200);
}

function stopCameraPreview() {
  if (cameraPreviewTimer) {
    clearInterval(cameraPreviewTimer);
    cameraPreviewTimer = null;
  }
}

/* 构造当前 Demo 自带的音频控制面板，页面不再创建 8092 外部 iframe。 */
function initializeAudioPage() {
  const page = document.getElementById('audio-page');
  page.innerHTML = `
    <section class="card"><h2>音频状态</h2>
      <button id="audio-refresh">刷新状态</button><pre id="audio-status">正在读取...</pre></section>
    <section class="card"><h2>左右麦录音</h2>
      <button id="record-left">开始左麦录音</button><button id="record-right">开始右麦录音</button>
      <button id="record-stop" class="danger">停止录音</button><button id="record-play">直接播放录音</button><button id="record-download">下载 WAV 录音</button></section>
    <section class="card"><h2>语音播放</h2>
      <label>PCM WAV 语音文件 <input id="voice-file" type="file" accept="audio/wav,.wav"></label>
      <button id="voice-upload">上传语音</button><button id="voice-play">播放语音</button>
      <button id="voice-stop" class="danger">停止播放</button></section>
    <section class="card"><h2>频率测试音</h2>
      <form id="tone-form"><label>频率 Hz <input id="tone-frequency" type="number" min="1" max="8000" value="1000"></label>
      <label>时长 ms <input id="tone-duration" type="number" min="50" max="10000" value="1000"></label>
      <label>播放音量 <input id="tone-volume" type="number" min="0" max="100" value="80"></label>
      <label>循环次数 <input id="tone-loops" type="number" min="1" max="1000" value="1"></label>
      <label class="checkbox"><input id="tone-infinite" type="checkbox"> 无限循环</label><button type="submit">播放测试音</button></form>
      <button id="tone-stop" class="danger">停止测试音</button></section>
    <section class="card"><h2>增益和音量</h2>
      <form id="audio-level-form"><label>麦克风增益 <input id="audio-gain" type="number" min="0" max="100" value="30"></label>
      <label>播放音量 <input id="audio-volume" type="number" min="0" max="100" value="80"></label><button type="submit">应用参数</button></form></section>`;

  const audioApi = async (path, options = {}) => {
    const response = await fetch(path, { cache: 'no-store', ...options });
    const value = await response.json();
    if (!response.ok || value.ok === false) throw value;
    return value;
  };
  const refreshAudio = async () => {
    try { document.getElementById('audio-status').textContent = JSON.stringify(await audioApi('/api/v1/audio/status'), null, 2); }
    catch (error) { document.getElementById('audio-status').textContent = JSON.stringify(error, null, 2); }
  };
  const postAudio = (path, body) => audioApi(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  const startRecord = async (channel) => { try { await postAudio('/api/v1/audio/record/start', { channel }); await refreshAudio(); } catch (error) { showResult(error); } };
  document.getElementById('audio-refresh').onclick = refreshAudio;
  document.getElementById('record-left').onclick = () => startRecord('left');
  document.getElementById('record-right').onclick = () => startRecord('right');
  document.getElementById('record-stop').onclick = async () => { try { await postAudio('/api/v1/audio/record/stop', {}); await refreshAudio(); } catch (error) { showResult(error); } };
  document.getElementById('record-play').onclick = async () => { try { await postAudio('/api/v1/audio/record/play', { loop_count: 1 }); await refreshAudio(); } catch (error) { showResult(error); } };
  document.getElementById('record-download').onclick = async () => {
    const response = await fetch('/api/v1/audio/record/download', { cache: 'no-store' });
    if (!response.ok) { showResult(await response.json()); return; }
    const link = document.createElement('a'); link.href = URL.createObjectURL(await response.blob()); link.download = 'hardware_test_recording.ogg'; link.click();
  };
  document.getElementById('voice-upload').onclick = async () => {
    const file = document.getElementById('voice-file').files[0];
    if (!file) { showResult({ ok: false, message: '请先选择 WAV 语音文件' }); return; }
    try { showResult(await audioApi('/api/v1/audio/upload', { method: 'POST', body: await file.arrayBuffer() })); }
    catch (error) { showResult(error); }
  };
  document.getElementById('voice-play').onclick = async () => { try { await postAudio('/api/v1/audio/play', { source: 'voice', loop_count: 1 }); await refreshAudio(); } catch (error) { showResult(error); } };
  document.getElementById('voice-stop').onclick = () => postAudio('/api/v1/audio/stop', {}).catch(showResult);
  document.getElementById('tone-form').onsubmit = async (event) => {
    event.preventDefault();
    try { await postAudio('/api/v1/audio/play', { source: 'tone', frequency_hz: Number(document.getElementById('tone-frequency').value), duration_ms: Number(document.getElementById('tone-duration').value), volume: Number(document.getElementById('tone-volume').value), loop_count: Number(document.getElementById('tone-loops').value), infinite_loop: document.getElementById('tone-infinite').checked }); await refreshAudio(); }
    catch (error) { showResult(error); }
  };
  document.getElementById('tone-stop').onclick = () => postAudio('/api/v1/audio/stop', {}).catch(showResult);
  document.getElementById('audio-level-form').onsubmit = async (event) => {
    event.preventDefault();
    try { await postAudio('/api/v1/audio/gain', { gain: Number(document.getElementById('audio-gain').value) }); await postAudio('/api/v1/audio/volume', { volume: Number(document.getElementById('audio-volume').value) }); await refreshAudio(); }
    catch (error) { showResult(error); }
  };
  refreshAudio();
}

/* 读取当前板端状态，并把接口返回原样格式化显示。 */
async function refreshStatus() {
  try {
    const value = await api('/api/v1/wifi/status');
    document.getElementById('status-output').textContent = JSON.stringify(value.data, null, 2);
  } catch (error) {
    document.getElementById('status-output').textContent = JSON.stringify(error, null, 2);
    showResult(error);
  }
}

/* 读取保存网络列表；后端不会返回密码。 */
async function refreshSaved() {
  const list = document.getElementById('saved-list');
  try {
    const value = await api('/api/v1/wifi/saved');
    list.replaceChildren();
    for (const network of value.data.networks) {
      const item = document.createElement('div');
      item.className = 'list-item';
      const text = document.createElement('span');
      text.textContent = `ID ${network.network_id} | SSID ${network.ssid} | ${network.flags}`;
      const connect = document.createElement('button');
      connect.textContent = '连接';
      connect.onclick = () => connectNetwork(network.network_id);
      const remove = document.createElement('button');
      remove.textContent = '删除';
      remove.className = 'danger';
      remove.onclick = () => deleteNetwork(network.network_id);
      item.append(text, connect, remove);
      list.appendChild(item);
    }
  } catch (error) {
    list.textContent = JSON.stringify(error);
    showResult(error);
  }
}

/* 发起只读扫描；后端不会执行断开、停止或切换网络操作。 */
async function scan() {
  try {
    const value = await api('/api/v1/wifi/scan', { method: 'POST', body: '{}' });
    const list = document.getElementById('scan-list');
    list.replaceChildren();
    for (const ap of value.data.aps) {
      const item = document.createElement('div');
      item.className = 'list-item';
      item.textContent = `${ap.ssid || '(隐藏网络)'} | ${ap.signal_dbm} dBm | ${ap.frequency_mhz} MHz | ${ap.flags}`;
      list.appendChild(item);
    }
    showResult(value);
    await refreshStatus();
  } catch (error) {
    showResult(error);
  }
}

/* 保存网络并按用户勾选项决定是否执行连接切换。 */
async function addNetwork(event) {
  event.preventDefault();
  const auth = document.getElementById('auth').value;
  const body = {
    ssid: document.getElementById('ssid').value,
    password: document.getElementById('password').value,
    auth,
    connect_now: document.getElementById('connect-now').checked
  };
  try {
    const value = await api('/api/v1/wifi/saved', { method: 'POST', body: JSON.stringify(body) });
    showResult(value);
    document.getElementById('password').value = '';
    await refreshSaved();
    await refreshStatus();
  } catch (error) {
    showResult(error);
  }
}

/* 连接已经保存的网络；该操作明确会切换板端 WiFi。 */
async function connectNetwork(networkId) {
  try {
    const value = await api('/api/v1/wifi/connect', {
      method: 'POST',
      body: JSON.stringify({ network_id: networkId })
    });
    showResult(value);
    await refreshStatus();
  } catch (error) {
    showResult(error);
  }
}

/* 删除网络前由后端阻止删除当前正在使用的网络。 */
async function deleteNetwork(networkId) {
  if (!window.confirm(`确认删除网络 ID ${networkId}？`)) return;
  try {
    const value = await api(`/api/v1/wifi/saved/${encodeURIComponent(networkId)}`, { method: 'DELETE' });
    showResult(value);
    await refreshSaved();
  } catch (error) {
    showResult(error);
  }
}

document.getElementById('refresh-status').onclick = refreshStatus;
document.getElementById('refresh-saved').onclick = refreshSaved;
document.getElementById('scan').onclick = scan;
document.getElementById('add-form').onsubmit = addNetwork;
initializeAudioPage();
document.querySelectorAll('.nav-item').forEach((item) => {
  item.onclick = () => switchPage(item.dataset.page);
});
const openAudio = document.getElementById('open-audio');
if (openAudio) openAudio.onclick = () => switchPage('audio-page');
document.getElementById('camera-start').onclick = () => { startCameraPreview(); refreshCameraStatus(); };
document.getElementById('camera-stop').onclick = stopCameraPreview;
refreshStatus();
refreshSaved();
refreshCameraStatus();
