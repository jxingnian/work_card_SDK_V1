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

/* 调用蓝牙接口并把返回值原样显示，便于核对 SDK 返回码和设备状态。 */
async function bluetoothApi(path, options = {}) {
  const response = await fetch(path, {
    cache: 'no-store',
    headers: { 'Content-Type': 'application/json' },
    ...options
  });
  const value = await response.json();
  if (!response.ok || value.ok === false) throw value;
  return value;
}

/* 刷新蓝牙适配器状态。 */
async function refreshBluetoothStatus() {
  try {
    const value = await bluetoothApi('/api/v1/bluetooth/status');
    document.getElementById('bluetooth-status').textContent = JSON.stringify(value, null, 2);
    setBluetoothScanIndicator(Boolean(value.scanning));
  } catch (error) {
    document.getElementById('bluetooth-status').textContent = JSON.stringify(error, null, 2);
    setBluetoothScanIndicator(false);
  }
}

/* 显示蓝牙扫描的真实状态，让操作者能立即确认按钮是否已经生效。 */
function setBluetoothScanIndicator(scanning) {
  const indicator = document.getElementById('bluetooth-scan-indicator');
  if (!indicator) return;
  indicator.classList.toggle('active', scanning);
  indicator.innerHTML = `<span class="scan-dot"></span>${scanning ? '正在扫描' : '未扫描'}`;
  const start = document.getElementById('bluetooth-scan-start');
  const stop = document.getElementById('bluetooth-scan-stop');
  if (start) start.disabled = scanning;
  if (stop) stop.disabled = !scanning;
}

/* 统一显示蓝牙操作的发送、成功和失败状态。 */
function setBluetoothActionStatus(message, kind = '') {
  const status = document.getElementById('bluetooth-action-status');
  if (!status) return;
  status.textContent = message;
  status.className = `action-status${kind ? ` ${kind}` : ''}`;
}

/* 根据设备状态生成基础管理按钮，并将地址作为 JSON 参数发送到后端。 */
function bluetoothAction(address, action, enabled = true) {
  return bluetoothApi('/api/v1/bluetooth/action', {
    method: 'POST',
    body: JSON.stringify({ address, action, enabled })
  });
}

/* 将扫描到的设备设置为当前协议和音频测试目标，避免操作者手动复制 MAC 地址。 */
function selectBluetoothTarget(address) {
  document.getElementById('bluetooth-target-address').value = address;
}

/* 一次完成配对、信任和连接，保证后续 A2DP 播放使用当前设备。 */
async function pairAndConnectBluetoothDevice(device) {
  const resultElement = document.getElementById('bluetooth-result');
  selectBluetoothTarget(device.address);
  try {
    if (!device.connected) {
      /* 已配对设备不重复执行 pair，避免 BlueZ 返回重复配对错误。 */
      if (!device.paired) await bluetoothAction(device.address, 'pair');
      await bluetoothAction(device.address, 'trust', true);
      await bluetoothAction(device.address, 'connect');
    }
    resultElement.textContent = JSON.stringify({
      ok: true,
      message: '设备已配对并连接，已设为当前蓝牙音频目标',
      address: device.address
    }, null, 2);
    await refreshBluetoothDevices();
  } catch (error) {
    resultElement.textContent = JSON.stringify(error, null, 2);
  }
}

/* 刷新扫描设备列表，并显示配对、信任、连接和删除操作。 */
let bluetoothDevicesRefreshing = false;
async function refreshBluetoothDevices() {
  /* 定时器不能在上一次查询未完成时再次发起查询，避免请求堆积。 */
  if (bluetoothDevicesRefreshing) return;
  bluetoothDevicesRefreshing = true;
  const list = document.getElementById('bluetooth-device-list');
  try {
    const value = await bluetoothApi('/api/v1/bluetooth/devices');
    list.replaceChildren();
    if (!value.devices.length) {
      list.textContent = '当前没有发现设备，请开始扫描。';
      return;
    }
    for (const device of value.devices) {
      const item = document.createElement('div');
      item.className = 'list-item bluetooth-device';
      const info = document.createElement('span');
      info.textContent = `${device.name || '(无名称)'} | ${device.address} | RSSI ${device.rssi} dBm | ${device.le ? 'BLE' : 'Classic'}`;
      const actions = document.createElement('div');
      const addButton = (text, action, enabled = true, danger = false) => {
        const button = document.createElement('button');
        button.textContent = text;
        if (danger) button.className = 'danger';
        button.onclick = async () => {
          try {
            const result = await bluetoothAction(device.address, action, enabled);
            document.getElementById('bluetooth-result').textContent = JSON.stringify(result, null, 2);
            await refreshBluetoothDevices();
          } catch (error) {
            document.getElementById('bluetooth-result').textContent = JSON.stringify(error, null, 2);
          }
        };
        actions.appendChild(button);
      };
      const pairConnectButton = document.createElement('button');
      pairConnectButton.textContent = device.connected ? '选择当前设备' : '一键配对并连接';
      pairConnectButton.onclick = () => pairAndConnectBluetoothDevice(device);
      actions.appendChild(pairConnectButton);
      if (device.connected) addButton('断开', 'disconnect', true, true);
      else addButton('连接', 'connect');
      if (device.paired) addButton('取消配对', 'cancel_pair', true, true);
      else addButton('配对', 'pair');
      addButton(device.trusted ? '取消信任' : '信任', 'trust', !device.trusted);
      addButton('删除', 'remove', true, true);
      item.append(info, actions);
      list.appendChild(item);
    }
  } catch (error) {
    list.textContent = JSON.stringify(error, null, 2);
  } finally {
    bluetoothDevicesRefreshing = false;
  }
}

/* 开始扫描后定时刷新设备列表，停止扫描时清理定时器。 */
let bluetoothRefreshTimer = null;
async function startBluetoothScan() {
  const button = document.getElementById('bluetooth-scan-start');
  if (button.disabled) return;
  button.disabled = true;
  button.dataset.originalText = button.textContent;
  button.textContent = '启动中...';
  button.classList.add('button-busy');
  setBluetoothActionStatus('正在启动蓝牙扫描...', 'busy');
  try {
    const result = await bluetoothApi('/api/v1/bluetooth/scan/start', { method: 'POST', body: '{}' });
    document.getElementById('bluetooth-result').textContent = JSON.stringify(result, null, 2);
    setBluetoothScanIndicator(true);
    setBluetoothActionStatus('扫描已启动，设备列表每 2 秒自动刷新。', 'success');
    await refreshBluetoothStatus();
    await refreshBluetoothDevices();
    if (!bluetoothRefreshTimer) bluetoothRefreshTimer = setInterval(refreshBluetoothDevices, 2000);
  } catch (error) {
    document.getElementById('bluetooth-result').textContent = JSON.stringify(error, null, 2);
    setBluetoothScanIndicator(false);
    setBluetoothActionStatus(`扫描启动失败：${error.message || '请查看操作结果'}`, 'error');
  } finally {
    button.classList.remove('button-busy');
    button.textContent = button.dataset.originalText || '开始扫描';
    button.removeAttribute('data-original-text');
    button.disabled = Boolean(document.getElementById('bluetooth-scan-indicator')?.classList.contains('active'));
  }
}

async function stopBluetoothScan() {
  const button = document.getElementById('bluetooth-scan-stop');
  if (button.disabled) return;
  button.disabled = true;
  button.dataset.originalText = button.textContent;
  button.textContent = '停止中...';
  button.classList.add('button-busy');
  setBluetoothActionStatus('正在停止蓝牙扫描...', 'busy');
  try {
    const result = await bluetoothApi('/api/v1/bluetooth/scan/stop', { method: 'POST', body: '{}' });
    document.getElementById('bluetooth-result').textContent = JSON.stringify(result, null, 2);
    setBluetoothScanIndicator(false);
    setBluetoothActionStatus('扫描已停止。', 'success');
  } catch (error) {
    document.getElementById('bluetooth-result').textContent = JSON.stringify(error, null, 2);
    setBluetoothActionStatus(`停止扫描失败：${error.message || '请查看操作结果'}`, 'error');
  }
  if (bluetoothRefreshTimer) {
    clearInterval(bluetoothRefreshTimer);
    bluetoothRefreshTimer = null;
  }
  await refreshBluetoothStatus();
  button.classList.remove('button-busy');
  button.textContent = button.dataset.originalText || '停止扫描';
  button.removeAttribute('data-original-text');
  button.disabled = !document.getElementById('bluetooth-scan-indicator')?.classList.contains('active');
}

/* 只在蓝牙页面打开时读取状态，避免其他硬件页面产生无关轮询。 */
function initializeBluetoothPage() {
  document.getElementById('bluetooth-refresh').onclick = refreshBluetoothStatus;
  document.getElementById('bluetooth-devices-refresh').onclick = refreshBluetoothDevices;
  document.getElementById('bluetooth-scan-start').onclick = startBluetoothScan;
  document.getElementById('bluetooth-scan-stop').onclick = stopBluetoothScan;
  document.getElementById('bluetooth-scan-start').dataset.noButtonFeedback = 'true';
  document.getElementById('bluetooth-scan-stop').dataset.noButtonFeedback = 'true';

  /* 所有蓝牙按钮点击后立即显示处理中，动态生成的设备按钮同样覆盖。 */
  document.getElementById('bluetooth-page').addEventListener('click', (event) => {
    const button = event.target.closest('button');
    if (!button || button.dataset.noButtonFeedback === 'true') return;
    if (button.disabled || button.dataset.feedbackBusy === 'true') return;
    button.dataset.feedbackBusy = 'true';
    button.dataset.feedbackOriginal = button.textContent;
    button.textContent = '处理中...';
    button.classList.add('button-busy');
    setBluetoothActionStatus(`已点击“${button.dataset.feedbackOriginal}”，正在等待板端响应...`, 'busy');
    window.setTimeout(() => {
      if (button.dataset.feedbackBusy !== 'true') return;
      button.textContent = button.dataset.feedbackOriginal;
      button.classList.remove('button-busy');
      delete button.dataset.feedbackBusy;
      delete button.dataset.feedbackOriginal;
    }, 12000);
  });

  /* 统一读取第二阶段测试所需的设备地址、特征 UUID 和输入参数。 */
  const targetAddress = () => document.getElementById('bluetooth-target-address').value.trim();
  const characteristic = () => document.getElementById('gatt-characteristic').value.trim();
  const protocolResult = (value) => {
    document.getElementById('bluetooth-result').textContent = JSON.stringify(value, null, 2);
  };
  const postProtocol = async (path, body) => {
    setBluetoothActionStatus('协议操作已发送，等待板端响应...', 'busy');
    try {
      const value = await bluetoothApi(path, { method: 'POST', body: JSON.stringify(body) });
      protocolResult(value);
      setBluetoothActionStatus('协议操作已完成。', 'success');
    } catch (error) {
      protocolResult(error);
      setBluetoothActionStatus(`协议操作失败：${error.message || '请查看操作结果'}`, 'error');
    }
  };
  /* 通过 A2DP 将 Demo 自带的固定测试语音发送到当前目标设备。 */
  document.getElementById('bluetooth-test-voice').onclick = () => postProtocol(
    '/api/v1/bluetooth/test_voice', { address: targetAddress() });
  const gatt = (action) => postProtocol('/api/v1/bluetooth/gatt', {
    action, address: targetAddress(), characteristic: characteristic(),
    data_hex: document.getElementById('gatt-data').value.trim()
  });
  document.getElementById('gatt-discover').onclick = () => gatt('discover');
  document.getElementById('gatt-read').onclick = () => gatt('read');
  document.getElementById('gatt-write').onclick = () => gatt('write');
  document.getElementById('gatt-notify-start').onclick = () => gatt('notify_start');
  document.getElementById('gatt-notify-stop').onclick = () => gatt('notify_stop');

  /* A2DP 操作全部走同一个接口，接口返回 SDK 原始错误结果。 */
  const a2dp = (action, extra = {}) => postProtocol('/api/v1/bluetooth/a2dp', {
    action, address: targetAddress(), ...extra
  });
  document.getElementById('a2dp-info').onclick = () => a2dp('info');
  document.getElementById('a2dp-volume-set').onclick = () => a2dp('volume', { volume: Number(document.getElementById('a2dp-volume').value) });
  document.getElementById('a2dp-mute').onclick = () => a2dp('mute', { muted: true });
  document.getElementById('a2dp-unmute').onclick = () => a2dp('mute', { muted: false });
  document.getElementById('a2dp-play').onclick = () => a2dp('play', { path: document.getElementById('a2dp-path').value.trim() });
  document.getElementById('a2dp-stop').onclick = () => a2dp('stop');

  /* HFP 的 value 用于号码、AT 指令和通话静音布尔值。 */
  const hfp = (action, value = '') => postProtocol('/api/v1/bluetooth/hfp', {
    action, address: targetAddress(), value
  });
  document.getElementById('hfp-open-sco').onclick = () => hfp('open_sco');
  document.getElementById('hfp-close-sco').onclick = () => hfp('close_sco');
  document.getElementById('hfp-send-at').onclick = () => hfp('send_at', document.getElementById('hfp-value').value.trim());
  document.getElementById('hfp-dial').onclick = () => hfp('dial', document.getElementById('hfp-value').value.trim());
  document.getElementById('hfp-accept').onclick = () => hfp('accept');
  document.getElementById('hfp-reject').onclick = () => hfp('reject');
  document.getElementById('hfp-hangup').onclick = () => hfp('hangup');
  document.getElementById('hfp-hold').onclick = () => hfp('hold');
  document.getElementById('hfp-mute').onclick = () => hfp('mute', 'true');

  /* AVRCP 只需要目标设备地址和控制动作。 */
  const avrcp = (action) => postProtocol('/api/v1/bluetooth/avrcp', {
    action, address: targetAddress()
  });
  ['play', 'pause', 'stop', 'previous', 'next'].forEach((action) => {
    document.getElementById(`avrcp-${action}`).onclick = () => avrcp(action);
  });
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

/* 显示 4G 页面操作状态，让网口、定位和测速按钮都有即时反馈。 */
function setNetworkActionStatus(message, kind = '') {
  const status = document.getElementById('network-4g-action-status');
  if (!status) return;
  status.textContent = message;
  status.className = `action-status${kind ? ` ${kind}` : ''}`;
}

/* 读取 CH390D 连接的以太网口状态，不改变任何网络配置。 */
async function refreshNetworkStatus() {
  setNetworkActionStatus('正在读取以太网状态...', 'busy');
  try {
    const value = await api('/api/v1/ethernet/status');
    document.getElementById('network-4g-status').textContent = JSON.stringify(value.data, null, 2);
    setNetworkActionStatus(value.data.link_up ? '以太网链路已连接' : '以太网链路未连接', value.data.link_up ? 'success' : 'error');
  } catch (error) {
    document.getElementById('network-4g-status').textContent = JSON.stringify(error, null, 2);
    setNetworkActionStatus(`以太网状态读取失败：${error.message || '请查看结果'}`, 'error');
  }
}

/* 分开展示 GNSS 和 LBS 结果，避免把高精度和基站定位混为一个位置。 */
function renderLocation(value) {
  const data = value.data || {};
  document.getElementById('network-gnss-location').textContent = JSON.stringify(data.gnss || { valid: false, message: '无 GNSS 数据' }, null, 2);
  document.getElementById('network-lbs-location').textContent = JSON.stringify(data.lbs || { valid: false, message: '无 LBS 数据' }, null, 2);
  document.getElementById('network-location-result').textContent = JSON.stringify(value, null, 2);
}

/* 请求 CH390D 定位接口，成功后同时刷新 GNSS、LBS 和原始响应。 */
async function refreshNetworkLocation() {
  setNetworkActionStatus('正在请求 CH390D 定位...', 'busy');
  try {
    const value = await api('/api/v1/4g/location/refresh', { method: 'POST', body: '{}' });
    renderLocation(value);
    setNetworkActionStatus('定位信息获取成功', 'success');
  } catch (error) {
    document.getElementById('network-location-result').textContent = JSON.stringify(error, null, 2);
    setNetworkActionStatus(`定位获取失败：${error.message || '请检查 CH390D 网络'}`, 'error');
  }
}

let networkSpeedTimer = null;
let networkSpeedBusy = false;

/* 轮询后台测速状态，显示实时字节数、速度和最终结果。 */
async function refreshNetworkSpeed() {
  if (networkSpeedBusy) return;
  networkSpeedBusy = true;
  try {
    const value = await api('/api/v1/ethernet/speed/status');
    document.getElementById('network-speed-result').textContent = JSON.stringify(value.data, null, 2);
    if (value.data.running) {
      document.getElementById('network-speed-action-status').textContent = '测速进行中，请等待或点击停止';
    } else if (value.data.stage === 'finished') {
      document.getElementById('network-speed-action-status').textContent = '测速完成';
      document.getElementById('network-speed-action-status').className = 'action-status success';
      if (networkSpeedTimer) { clearInterval(networkSpeedTimer); networkSpeedTimer = null; }
    }
  } catch (error) {
    document.getElementById('network-speed-result').textContent = JSON.stringify(error, null, 2);
  } finally {
    networkSpeedBusy = false;
  }
}

/* 启动绑定 eth0 的后台下载测速任务，避免重复启动测速线程。 */
async function startNetworkSpeed() {
  const button = document.getElementById('network-speed-start');
  button.disabled = true;
  document.getElementById('network-speed-action-status').textContent = '正在启动测速...';
  document.getElementById('network-speed-action-status').className = 'action-status busy';
  try {
    await api('/api/v1/ethernet/speed/start', {
      method: 'POST',
      body: JSON.stringify({ url: document.getElementById('network-speed-url').value.trim() })
    });
    if (!networkSpeedTimer) networkSpeedTimer = setInterval(refreshNetworkSpeed, 500);
    await refreshNetworkSpeed();
  } catch (error) {
    document.getElementById('network-speed-result').textContent = JSON.stringify(error, null, 2);
    document.getElementById('network-speed-action-status').textContent = `测速启动失败：${error.message || '请查看结果'}`;
    document.getElementById('network-speed-action-status').className = 'action-status error';
  } finally {
    button.disabled = false;
  }
}

/* 请求停止测速，后台线程会在下一次读操作时退出并返回已测量数据。 */
async function stopNetworkSpeed() {
  try {
    await api('/api/v1/ethernet/speed/stop', { method: 'POST', body: '{}' });
    document.getElementById('network-speed-action-status').textContent = '正在停止测速...';
    document.getElementById('network-speed-action-status').className = 'action-status busy';
    await refreshNetworkSpeed();
  } catch (error) {
    document.getElementById('network-speed-result').textContent = JSON.stringify(error, null, 2);
  }
}

/* 初始化 4G 页面按钮；页面切换不触发网络配置，只执行读状态和测试请求。 */
function initializeNetworkPage() {
  document.getElementById('network-4g-refresh').onclick = refreshNetworkStatus;
  document.getElementById('network-location-refresh').onclick = refreshNetworkLocation;
  document.getElementById('network-speed-start').onclick = startNetworkSpeed;
  document.getElementById('network-speed-stop').onclick = stopNetworkSpeed;
  refreshNetworkStatus();
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
initializeNetworkPage();
initializeAudioPage();
initializeBluetoothPage();
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
refreshBluetoothStatus();
