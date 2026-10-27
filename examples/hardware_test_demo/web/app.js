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
refreshStatus();
refreshSaved();
