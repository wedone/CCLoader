// CCLoader WebUI 前端逻辑
// 使用 SSE (EventSource) 接收实时事件，无外部库依赖

let es = null;
let esReconnectTimer = null;
let monitorPaused = false;
let monitorActive = false;
let monitorBytes = 0;
let monitorBuffer = '';  // 不完整行缓冲
let selectedFile = '';

// ===== 工具函数 =====
function $(id) { return document.getElementById(id); }

function pad(n, w) { return String(n).padStart(w, '0'); }

function timestamp() {
  const d = new Date();
  return pad(d.getHours(), 2) + ':' + pad(d.getMinutes(), 2) + ':' +
         pad(d.getSeconds(), 2) + '.' + pad(d.getMilliseconds(), 3);
}

function appendLog(area, text, className) {
  const div = document.createElement('div');
  div.className = 'log-line' + (className ? ' ' + className : '');
  div.textContent = '[' + timestamp() + '] ' + text;
  area.appendChild(div);
  // 限制 5000 行避免内存溢出
  while (area.children.length > 5000) {
    area.removeChild(area.firstChild);
  }
  area.scrollTop = area.scrollHeight;
}

// Base64 解码为字符串（UTF-8 安全）
function b64ToString(b64) {
  try {
    const bin = atob(b64);
    // 处理 UTF-8 多字节
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new TextDecoder('utf-8').decode(bytes);
  } catch (e) {
    return '';
  }
}

// ===== 状态更新 =====
function setStateBadge(state) {
  const badge = $('state-badge');
  badge.className = 'badge';
  if (state === 'idle') {
    badge.classList.add('idle');
    badge.textContent = '空闲';
  } else if (state === 'burning') {
    badge.classList.add('burning');
    badge.textContent = '烧录中';
  } else if (state === 'monitoring') {
    badge.classList.add('monitor');
    badge.textContent = '监控中';
  }
}

// ===== SSE 连接（端口 81）=====
function connectSSE() {
  if (es && es.readyState !== EventSource.CLOSED) {
    return;
  }
  // SSE 走 81 端口，与 HTTP 80 分开
  const url = 'http://' + location.hostname + ':81/';
  es = new EventSource(url);

  es.onopen = () => {
    $('ws-state').textContent = '已连接';
    if (esReconnectTimer) {
      clearTimeout(esReconnectTimer);
      esReconnectTimer = null;
    }
  };

  es.onmessage = (event) => {
    let msg;
    try { msg = JSON.parse(event.data); } catch (e) { return; }
    switch (msg.type) {
      case 'burn_progress':
        updateBurnProgress(msg);
        break;
      case 'monitor_start':
        onMonitorStart(msg.baud);
        break;
      case 'monitor_data':
        if (!monitorPaused) {
          appendMonitorData(b64ToString(msg.data));
        }
        break;
      case 'monitor_stop':
        onMonitorStop();
        break;
      case 'monitor_reset':
        onMonitorReset();
        break;
      case 'wifi_connected':
        if (msg.ip) {
          $('wifi-connect-status').innerHTML =
            '<strong style="color: var(--success)">连接成功！</strong> 新 IP: ' + msg.ip +
            '<br>请切换到 ' + msg.ssid + ' WiFi 后访问 http://' + msg.ip + '/';
        }
        break;
      case 'wifi_connect_failed':
        $('wifi-connect-status').innerHTML =
          '<strong style="color: var(--danger)">连接失败</strong>：' + (msg.ssid || '') +
          ' 密码错误或信号太弱，ESP8266 已切回 AP 模式';
        $('wifi-connect-btn').disabled = false;
        break;
      case 'status':
        if (msg.state) setStateBadge(msg.state);
        break;
    }
  };

  es.onerror = () => {
    $('ws-state').textContent = '已断开';
    // EventSource 会自动重连，但 ESP8266 断电后需手动兜底
    if (es.readyState === EventSource.CLOSED) {
      if (esReconnectTimer) clearTimeout(esReconnectTimer);
      esReconnectTimer = setTimeout(connectSSE, 2000);
    }
  };
}

// ===== 标签切换 =====
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
    btn.classList.add('active');
    $('tab-' + btn.dataset.tab).classList.add('active');
  });
});

// ===== hex2bin：Intel HEX 转 BIN（参考 diyruz_rt/Tools/hex2bin.py）=====
// 拆分为 parseHexToMap + mapToBin 两个函数，便于 OTA 分体固件合并复用

// 解析 HEX 文件到共享 dataMap
// - dataMap: Map<addr, Uint8Array>，可跨多个 hex 累积
// - addrRange: { min, max, countType0, countType2, countType4 }，函数内更新
// - log: 字符串数组，函数内追加
// 抛出异常时 dataMap/addrRange 可能已部分更新（调用方应丢弃）
async function parseHexToMap(file, dataMap, addrRange, log) {
  log.push('HEX 文件: ' + file.name);

  const text = await file.text();
  const lines = text.split(/\r?\n/);
  log.push('总行数: ' + lines.length);

  let baseAddr = 0x00000;

  for (let lineNo = 0; lineNo < lines.length; lineNo++) {
    const line = lines[lineNo].trim();
    if (!line || line[0] !== ':') continue;

    // 解析 hex 字节
    const raw = [];
    for (let i = 1; i + 1 < line.length; i += 2) {
      const b = parseInt(line.substr(i, 2), 16);
      if (isNaN(b)) throw new Error(file.name + ' line ' + (lineNo + 1) + ': 非法 hex 字符');
      raw.push(b);
    }
    if (raw.length < 5) continue;

    const bCount = raw[0];
    const bAddr = (raw[1] << 8) | raw[2];
    const bType = raw[3];
    const data = raw.slice(4, 4 + bCount);

    // 校验和验证
    let sum = 0;
    for (let i = 0; i < raw.length - 1; i++) sum = (sum + raw[i]) & 0xFF;
    const csum = raw[raw.length - 1];
    if (((sum + csum) & 0xFF) !== 0) {
      throw new Error(file.name + ' line ' + (lineNo + 1) + ': 校验和错误');
    }

    if (bType === 0x01) {
      // 结束记录
      break;
    } else if (bType === 0x02) {
      // 扩展段地址
      baseAddr = ((data[0] << 8) | data[1]) << 4;
      addrRange.countType2++;
    } else if (bType === 0x04) {
      // 扩展线性地址
      baseAddr = ((data[0] << 8) | data[1]) << 16;
      addrRange.countType4++;
    } else if (bType === 0x05) {
      // 起始线性地址，忽略
    } else if (bType === 0x00) {
      // 数据记录
      const physAddr = baseAddr + bAddr;
      dataMap.set(physAddr, new Uint8Array(data));
      if (addrRange.min === null || physAddr < addrRange.min) addrRange.min = physAddr;
      const endAddr = physAddr + data.length - 1;
      if (addrRange.max === null || endAddr > addrRange.max) addrRange.max = endAddr;
      addrRange.countType0++;
    } else {
      log.push('警告 ' + file.name + ' line ' + (lineNo + 1) + ': 未知记录类型 0x' + bType.toString(16));
    }
  }
}

// dataMap 转 256KB BIN（CC2530 要求完整 256KB）
// 返回 { bin, binSize, padBytes }
function mapToBin(dataMap, maxAddr) {
  const padTo = 0x40000;  // 256KB
  const binSize = Math.max(maxAddr + 1, padTo);
  const padBytes = binSize - (maxAddr + 1);

  const bin = new Uint8Array(binSize);
  bin.fill(0xFF);
  for (const [addr, data] of dataMap) {
    bin.set(data, addr);
  }
  return { bin: bin, binSize: binSize, padBytes: padBytes };
}

// 单文件 hex2bin（原有接口，行为不变）
// 返回 { bin: Uint8Array, log: [strings], name: string }
async function hex2bin(file) {
  const log = [];
  const dataMap = new Map();
  const addrRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };

  await parseHexToMap(file, dataMap, addrRange, log);

  if (addrRange.min === null) throw new Error('HEX 文件无数据记录');

  log.push('数据记录 type00: ' + addrRange.countType0);
  log.push('扩展线性地址 type04: ' + addrRange.countType4);
  log.push('扩展段地址 type02: ' + addrRange.countType2);
  log.push('地址范围: 0x' + addrRange.min.toString(16) + ' - 0x' + addrRange.max.toString(16));
  const dataSpan = addrRange.max - addrRange.min + 1;
  log.push('数据跨度: ' + dataSpan + ' 字节 (' + (dataSpan / 1024).toFixed(1) + ' KB)');

  const { bin, binSize, padBytes } = mapToBin(dataMap, addrRange.max);
  log.push('BIN 大小: ' + binSize + ' 字节 (' + (binSize / 1024).toFixed(1) + ' KB)');
  log.push('尾部填充 0xFF: ' + padBytes + ' 字节 (' + (padBytes / 1024).toFixed(1) + ' KB)');

  const baseName = file.name.replace(/\.hex$/i, '');
  return { bin: bin, log: log, name: baseName + '.bin' };
}

// ===== OTA 分体固件合并 =====
// 解析 Boot.hex + App.hex 到同一个 dataMap，生成合并 BIN
// 地址冲突检测：Bootloader 应在 0x0000~0x07FF，应用在 0x0800+，重叠则报错
// 返回 { bin, log, name: 'OTA_merged.bin', bootRange, appRange }
async function mergeOtaHex(bootFile, appFile) {
  const log = [];
  const dataMap = new Map();
  const addrRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };

  // 解析 Bootloader（先单独解析一次，记录地址范围）
  log.push('=== OTA Bootloader ===');
  const bootRange = { min: null, max: null };
  {
    const tmpMap = new Map();
    const tmpRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };
    await parseHexToMap(bootFile, tmpMap, tmpRange, log);
    if (tmpRange.min === null) throw new Error('Bootloader HEX 无数据记录');
    bootRange.min = tmpRange.min;
    bootRange.max = tmpRange.max;
    // 合并到主 dataMap
    for (const [a, d] of tmpMap) dataMap.set(a, d);
    if (addrRange.min === null || tmpRange.min < addrRange.min) addrRange.min = tmpRange.min;
    if (addrRange.max === null || tmpRange.max > addrRange.max) addrRange.max = tmpRange.max;
    addrRange.countType0 += tmpRange.countType0;
    addrRange.countType2 += tmpRange.countType2;
    addrRange.countType4 += tmpRange.countType4;
  }

  // 解析应用固件
  log.push('=== 应用固件 ===');
  const appRange = { min: null, max: null };
  {
    const tmpMap = new Map();
    const tmpRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };
    await parseHexToMap(appFile, tmpMap, tmpRange, log);
    if (tmpRange.min === null) throw new Error('应用固件 HEX 无数据记录');
    appRange.min = tmpRange.min;
    appRange.max = tmpRange.max;
    // 地址冲突检测：Bootloader 和应用不应重叠
    // Bootloader 在 0x0000~0x07FF，应用在 0x0800+，正常不重叠
    if (tmpRange.min <= bootRange.max) {
      throw new Error('地址冲突：应用固件起始 0x' + tmpRange.min.toString(16) +
                      ' <= Bootloader 结束 0x' + bootRange.max.toString(16) +
                      '，期望 Bootloader 在 0x0000~0x07FF，应用在 0x0800+');
    }
    // 合并到主 dataMap（地址不重叠，直接 set）
    for (const [a, d] of tmpMap) dataMap.set(a, d);
    if (tmpRange.max > addrRange.max) addrRange.max = tmpRange.max;
    addrRange.countType0 += tmpRange.countType0;
    addrRange.countType2 += tmpRange.countType2;
    addrRange.countType4 += tmpRange.countType4;
  }

  log.push('=== 合并结果 ===');
  log.push('Bootloader: 0x' + bootRange.min.toString(16) + ' ~ 0x' + bootRange.max.toString(16) +
           ' (' + (bootRange.max - bootRange.min + 1) + ' 字节)');
  log.push('应用固件:   0x' + appRange.min.toString(16) + ' ~ 0x' + appRange.max.toString(16) +
           ' (' + (appRange.max - appRange.min + 1) + ' 字节)');
  log.push('合并后地址范围: 0x' + addrRange.min.toString(16) + ' ~ 0x' + addrRange.max.toString(16));
  log.push('数据记录总数: ' + addrRange.countType0);

  const { bin, binSize, padBytes } = mapToBin(dataMap, addrRange.max);
  log.push('合并 BIN: ' + binSize + ' 字节 (' + (binSize / 1024).toFixed(1) + ' KB)');

  return {
    bin: bin,
    log: log,
    name: 'OTA_merged.bin',
    bootRange: bootRange,
    appRange: appRange
  };
}

// 轻量解析：只算地址范围，不构造 dataMap（用于文件选择时即时预览）
async function quickHexInfo(file) {
  const tmpMap = new Map();
  const tmpRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };
  await parseHexToMap(file, tmpMap, tmpRange, []);
  if (tmpRange.min === null) throw new Error(file.name + ': 无数据记录');
  return {
    min: tmpRange.min,
    max: tmpRange.max,
    span: tmpRange.max - tmpRange.min + 1
  };
}

// ===== 烧录模式切换 =====
document.querySelectorAll('input[name="burn-mode"]').forEach(r => {
  r.addEventListener('change', e => {
    document.getElementById('single-mode').style.display =
      e.target.value === 'single' ? 'block' : 'none';
    document.getElementById('ota-mode').style.display =
      e.target.value === 'ota' ? 'block' : 'none';
  });
});

// OTA 文件选择：两个文件都选好后即时预览地址范围
async function updateOtaPreview() {
  const bootFile = $('ota-boot-file').files[0];
  const appFile = $('ota-app-file').files[0];
  const btn = $('ota-merge-upload-btn');
  const preview = $('ota-merge-preview');
  const bootInfo = $('ota-boot-info');
  const appInfo = $('ota-app-info');

  bootInfo.textContent = '';
  appInfo.textContent = '';
  preview.textContent = '';
  preview.classList.remove('error');
  btn.disabled = true;

  if (bootFile) {
    try {
      const info = await quickHexInfo(bootFile);
      bootInfo.textContent = '0x' + info.min.toString(16) + ' ~ 0x' + info.max.toString(16) +
                            ' (' + (info.span / 1024).toFixed(1) + ' KB)';
    } catch (e) {
      bootInfo.textContent = '解析失败: ' + e.message;
    }
  }
  if (appFile) {
    try {
      const info = await quickHexInfo(appFile);
      appInfo.textContent = '0x' + info.min.toString(16) + ' ~ 0x' + info.max.toString(16) +
                           ' (' + (info.span / 1024).toFixed(1) + ' KB)';
    } catch (e) {
      appInfo.textContent = '解析失败: ' + e.message;
    }
  }

  if (!bootFile || !appFile) return;

  // 完整预览
  try {
    const bootInfo2 = await quickHexInfo(bootFile);
    const appInfo2 = await quickHexInfo(appFile);
    const overlap = appInfo2.min <= bootInfo2.max;
    const bootClass = (bootInfo2.min === 0 && bootInfo2.max <= 0x07FF) ? 'addr-ok' : 'addr-warn';
    const appClass = (appInfo2.min >= 0x0800) ? 'addr-ok' : 'addr-warn';
    let html = 'Bootloader  0x' + bootInfo2.min.toString(16).padStart(4, '0') +
               ' ~ 0x' + bootInfo2.max.toString(16).padStart(4, '0') +
               '  (' + (bootInfo2.span / 1024).toFixed(1) + ' KB)\n';
    html += '应用固件    0x' + appInfo2.min.toString(16).padStart(4, '0') +
            ' ~ 0x' + appInfo2.max.toString(16).padStart(4, '0') +
            '  (' + (appInfo2.span / 1024).toFixed(1) + ' KB)\n';
    html += '合并后 BIN  256 KB';
    if (overlap) {
      html += '\n⚠️ 地址重叠：应用起始 0x' + appInfo2.min.toString(16) +
              ' <= Bootloader 结束 0x' + bootInfo2.max.toString(16);
      preview.classList.add('error');
      btn.disabled = true;
    } else {
      btn.disabled = false;
    }
    preview.textContent = html;
  } catch (e) {
    preview.textContent = '预览失败: ' + e.message;
    preview.classList.add('error');
  }
}

$('ota-boot-file').addEventListener('change', updateOtaPreview);
$('ota-app-file').addEventListener('change', updateOtaPreview);

// 合并并上传
$('ota-merge-upload-btn').addEventListener('click', async () => {
  const bootFile = $('ota-boot-file').files[0];
  const appFile = $('ota-app-file').files[0];
  if (!bootFile || !appFile) {
    alert('请选择两个 .hex 文件');
    return;
  }
  const btn = $('ota-merge-upload-btn');
  const progress = $('ota-merge-progress');
  btn.disabled = true;
  progress.textContent = '合并中...';

  try {
    const result = await mergeOtaHex(bootFile, appFile);
    progress.innerHTML = result.log.join('<br>') + '<br>上传中...';

    const blob = new Blob([result.bin], { type: 'application/octet-stream' });
    const formData = new FormData();
    formData.append('file', blob, result.name);

    const resp = await fetch('/api/upload', { method: 'POST', body: formData });
    const data = await resp.json();
    if (data.success) {
      progress.innerHTML = result.log.join('<br>') +
                           '<br><strong style="color: var(--success)">上传成功: ' +
                           data.filename + ' (' + data.size + ' 字节)</strong>';
      refreshFileList();
      // 自动选中新上传的合并 BIN
      selectedFile = data.filename;
      $('selected-file').textContent = selectedFile;
      setTimeout(refreshFileList, 100);  // 列表刷新后再标 selected
    } else {
      progress.innerHTML = result.log.join('<br>') +
                           '<br><strong style="color: var(--danger)">上传失败: ' +
                           (data.error || '未知错误') + '</strong>';
    }
  } catch (e) {
    progress.innerHTML = '<strong style="color: var(--danger)">合并失败: ' + e.message + '</strong>';
  } finally {
    btn.disabled = false;
  }
});

// ===== 文件上传 =====
$('upload-btn').addEventListener('click', async () => {
  const input = $('file-input');
  if (!input.files || input.files.length === 0) {
    alert('请先选择文件');
    return;
  }
  const file = input.files[0];
  const isHex = /\.hex$/i.test(file.name);

  $('upload-btn').disabled = true;

  try {
    let uploadFile;
    let uploadName;
    if (isHex) {
      $('upload-progress').textContent = 'HEX → BIN 转换中...';
      try {
        const result = await hex2bin(file);
        uploadFile = new Blob([result.bin], { type: 'application/octet-stream' });
        uploadName = result.name;
        // 显示转换日志
        $('upload-progress').innerHTML = result.log.join('<br>');
      } catch (e) {
        $('upload-progress').textContent = 'HEX 转换失败: ' + e.message;
        return;
      }
    } else {
      uploadFile = file;
      uploadName = file.name;
      $('upload-progress').textContent = '上传中...';
    }

    const formData = new FormData();
    formData.append('file', uploadFile, uploadName);

    if (!isHex) $('upload-progress').textContent = '上传中...';
    else $('upload-progress').innerHTML += '<br>上传中...';

    const resp = await fetch('/api/upload', { method: 'POST', body: formData });
    const result = await resp.json();
    if (result.success) {
      const html = isHex ? $('upload-progress').innerHTML + '<br>' : '';
      $('upload-progress').innerHTML = html + '上传成功: ' + result.filename + ' (' + result.size + ' 字节)';
      refreshFileList();
    } else {
      $('upload-progress').textContent = '上传失败: ' + (result.error || '未知错误');
    }
  } catch (e) {
    $('upload-progress').textContent = '上传失败: ' + e.message;
  } finally {
    $('upload-btn').disabled = false;
    input.value = '';
  }
});

// ===== 文件列表 =====
async function refreshFileList() {
  try {
    const resp = await fetch('/api/files');
    const result = await resp.json();
    if (!result.success) return;
    const list = $('file-list');
    list.innerHTML = '';
    if (result.files.length === 0) {
      list.innerHTML = '<div style="color: var(--text-muted)">暂无固件，请上传</div>';
      return;
    }
    result.files.forEach(f => {
      const item = document.createElement('div');
      item.className = 'file-item';
      if (f.name === selectedFile) item.classList.add('selected');
      const sizeKB = (f.size / 1024).toFixed(1);
      const date = new Date(f.time * 1000).toLocaleString('zh-CN');

      const info = document.createElement('div');
      info.className = 'file-info';
      const nameSpan = document.createElement('span');
      nameSpan.className = 'file-name';
      nameSpan.textContent = f.name;  // textContent 防 XSS
      const sizeSpan = document.createElement('span');
      sizeSpan.className = 'file-meta';
      sizeSpan.textContent = sizeKB + ' KB';
      const dateSpan = document.createElement('span');
      dateSpan.className = 'file-meta';
      dateSpan.textContent = date;
      info.appendChild(nameSpan);
      info.appendChild(sizeSpan);
      info.appendChild(dateSpan);

      const delBtn = document.createElement('button');
      delBtn.className = 'btn danger delete-btn';
      delBtn.dataset.name = f.name;  // dataset 自动转义
      delBtn.textContent = '删除';

      item.appendChild(info);
      item.appendChild(delBtn);
      list.appendChild(item);
    });
    // 选中事件
    list.querySelectorAll('.file-name').forEach(el => {
      el.addEventListener('click', () => {
        selectedFile = el.textContent;
        $('selected-file').textContent = selectedFile;
        list.querySelectorAll('.file-item').forEach(i => i.classList.remove('selected'));
        el.closest('.file-item').classList.add('selected');
      });
    });
    // 删除事件
    list.querySelectorAll('.delete-btn').forEach(btn => {
      btn.addEventListener('click', async () => {
        if (!confirm('删除 ' + btn.dataset.name + '?')) return;
        const resp = await fetch('/api/files/' + encodeURIComponent(btn.dataset.name), { method: 'DELETE' });
        const r = await resp.json();
        if (r.success) {
          if (selectedFile === btn.dataset.name) {
            selectedFile = '';
            $('selected-file').textContent = '未选择';
          }
          refreshFileList();
        } else {
          alert('删除失败: ' + r.error);
        }
      });
    });
  } catch (e) {
    console.error('refreshFileList error:', e);
  }
}

// ===== 烧录 =====
$('burn-btn').addEventListener('click', async () => {
  if (!selectedFile) {
    alert('请先选择固件');
    return;
  }
  const verify = $('verify-check').checked;
  $('burn-btn').disabled = true;
  $('burn-log').innerHTML = '';
  $('burn-progress-bar').style.width = '0%';
  $('burn-progress-text').textContent = '0%';
  appendLog($('burn-log'), '开始烧录: ' + selectedFile + (verify ? ' (校验)' : ''), 'success');

  try {
    const resp = await fetch('/api/burn', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ filename: selectedFile, verify: verify })
    });
    const result = await resp.json();
    if (!result.success) {
      appendLog($('burn-log'), '启动失败: ' + result.error, 'error');
      $('burn-btn').disabled = false;
    } else {
      appendLog($('burn-log'), '总块数: ' + result.total_blocks);
    }
  } catch (e) {
    appendLog($('burn-log'), '请求失败: ' + e.message, 'error');
    $('burn-btn').disabled = false;
  }
});

function updateBurnProgress(msg) {
  $('burn-progress-bar').style.width = msg.percent + '%';
  $('burn-progress-text').textContent = msg.percent + '% (' + msg.current_block + '/' + msg.total_blocks + ')';
  if (msg.current_block > 0 && msg.current_block % 50 === 0 && msg.current_block < msg.total_blocks) {
    appendLog($('burn-log'), '写入块 ' + msg.current_block + '/' + msg.total_blocks);
  }
  if (msg.error) {
    appendLog($('burn-log'), '错误: ' + msg.error, 'error');
  }
  if (msg.done) {
    appendLog($('burn-log'), '烧录完成', 'success');
    $('burn-btn').disabled = false;
    // 烧录完成后不再自动跳到监控页，由用户手动切换
    // 勾选"烧录完成重启烧录器"时调用 /api/reboot 重启 ESP8266
    if ($('reboot-burner-check').checked) {
      appendLog($('burn-log'), '正在重启烧录器...', 'success');
      fetch('/api/reboot', { method: 'POST' }).catch(() => {});
    }
  }
}

// ===== 监控 =====
$('monitor-start-btn').addEventListener('click', async () => {
  const baud = parseInt($('baud-select').value);
  const autoReset = $('auto-reset-check').checked;
  $('monitor-start-btn').disabled = true;
  try {
    const resp = await fetch('/api/monitor', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ baud: baud, auto_reset: autoReset })
    });
    const result = await resp.json();
    if (!result.success) {
      alert('启动监控失败: ' + result.error);
      $('monitor-start-btn').disabled = false;
    }
    // 等待 monitor_start SSE 事件再切按钮状态
  } catch (e) {
    alert('请求失败: ' + e.message);
    $('monitor-start-btn').disabled = false;
  }
});

// 手动复位 CC2530（监控中也可用，复位后从 main() 重新输出日志）
$('reset-cc-btn').addEventListener('click', async () => {
  $('reset-cc-btn').disabled = true;
  try {
    const resp = await fetch('/api/reset', { method: 'POST' });
    const result = await resp.json();
    if (result.success) {
      // 监控中复位会收到 monitor_reset 事件清空日志
      // 非监控中只复位硬件
    } else {
      alert('复位失败: ' + (result.error || '未知错误'));
    }
  } catch (e) {
    alert('请求失败: ' + e.message);
  } finally {
    $('reset-cc-btn').disabled = false;
  }
});

$('monitor-stop-btn').addEventListener('click', async () => {
  try {
    await fetch('/api/stop', { method: 'POST' });
  } catch (e) {}
});

function onMonitorStart(baud) {
  monitorActive = true;
  monitorBytes = 0;
  monitorBuffer = '';
  $('bytes-received').textContent = '0';
  $('monitor-log').innerHTML = '';
  $('monitor-state').textContent = '监控中 @ ' + baud + ' bps';
  $('monitor-start-btn').disabled = true;
  $('monitor-stop-btn').disabled = false;
  $('reset-cc-btn').disabled = false;
  $('pause-btn').disabled = false;
  $('clear-btn').disabled = false;
  $('download-btn').disabled = false;
  $('search-input').disabled = false;
  const autoReset = $('auto-reset-check').checked;
  if (autoReset) {
    appendLog($('monitor-log'), '监控开始 @ ' + baud + ' bps（已自动复位 CC2530）', 'success');
  } else {
    appendLog($('monitor-log'), '监控开始 @ ' + baud + ' bps，点"复位 CC2530"看启动日志', 'success');
  }
}

function onMonitorStop() {
  monitorActive = false;
  $('monitor-state').textContent = '已停止';
  $('monitor-start-btn').disabled = false;
  $('monitor-stop-btn').disabled = true;
  $('reset-cc-btn').disabled = true;
  $('pause-btn').disabled = true;
  $('pause-btn').textContent = '暂停';
  appendLog($('monitor-log'), '监控已停止', 'error');
}

// 监控中收到 monitor_reset 事件：CC2530 已复位，清空日志区准备接收启动日志
function onMonitorReset() {
  monitorBytes = 0;
  monitorBuffer = '';
  $('bytes-received').textContent = '0';
  $('monitor-log').innerHTML = '';
  appendLog($('monitor-log'), 'CC2530 已复位，等待启动日志...', 'success');
}

function appendMonitorData(data) {
  monitorBytes += data.length;
  $('bytes-received').textContent = monitorBytes;
  monitorBuffer += data;
  const log = $('monitor-log');
  // 按行分割
  let idx;
  while ((idx = monitorBuffer.indexOf('\n')) >= 0) {
    let line = monitorBuffer.substring(0, idx + 1);
    monitorBuffer = monitorBuffer.substring(idx + 1);
    // 搜索过滤
    const filter = $('search-input').value.trim();
    if (filter && line.indexOf(filter) < 0) continue;
    const div = document.createElement('div');
    div.className = 'log-line';
    div.textContent = '[' + timestamp() + '] ' + line.replace(/\r?\n$/, '');
    log.appendChild(div);
    while (log.children.length > 5000) log.removeChild(log.firstChild);
  }
  log.scrollTop = log.scrollHeight;
}

$('pause-btn').addEventListener('click', () => {
  monitorPaused = !monitorPaused;
  $('pause-btn').textContent = monitorPaused ? '继续' : '暂停';
});

$('clear-btn').addEventListener('click', () => {
  $('monitor-log').innerHTML = '';
  monitorBuffer = '';
});

$('download-btn').addEventListener('click', () => {
  const lines = [];
  $('monitor-log').querySelectorAll('.log-line').forEach(el => lines.push(el.textContent));
  const blob = new Blob([lines.join('\n')], { type: 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'cc2530_monitor_' + Date.now() + '.log';
  a.click();
  URL.revokeObjectURL(a.href);
});

$('search-input').addEventListener('input', () => {
  // 实时过滤由 appendMonitorData 处理；已显示的不重过滤（简化）
});

// ===== 设置 =====
async function loadConfig() {
  try {
    const resp = await fetch('/api/config');
    const cfg = await resp.json();
    $('wifi-ssid').value = cfg.wifi_ssid || '';
    $('wifi-password').value = cfg.wifi_password || '';
    $('default-baud-select').value = cfg.monitor_baud || 115200;
    $('default-verify-check').checked = !!cfg.verify;
    $('baud-select').value = cfg.monitor_baud || 115200;
    $('verify-check').checked = !!cfg.verify;
  } catch (e) {
    console.error('loadConfig error:', e);
  }
}

// ===== WiFi 配网 =====
function rssiClass(rssi) {
  if (rssi >= -55) return 'strong';
  if (rssi >= -75) return 'medium';
  return 'weak';
}

function rssiLabel(rssi) {
  if (rssi >= -55) return '强';
  if (rssi >= -75) return '中';
  return '弱';
}

async function scanWifi() {
  $('wifi-scan-btn').disabled = true;
  $('wifi-scan-status').textContent = '扫描中...';
  $('wifi-list').innerHTML = '';
  try {
    const resp = await fetch('/api/wifi/scan');
    const result = await resp.json();
    if (!result.success) {
      $('wifi-scan-status').textContent = '扫描失败: ' + (result.error || '未知错误');
      return;
    }
    const networks = result.networks || [];
    $('wifi-scan-status').textContent = '找到 ' + networks.length + ' 个网络';
    if (networks.length === 0) {
      $('wifi-list').innerHTML = '<div class="hint">未找到网络</div>';
      return;
    }
    const list = $('wifi-list');
    networks.forEach(n => {
      const item = document.createElement('div');
      item.className = 'wifi-item';
      const left = document.createElement('span');
      left.className = 'wifi-ssid';
      left.textContent = n.ssid;
      if (n.encrypted) {
        const lock = document.createElement('span');
        lock.className = 'wifi-lock';
        lock.textContent = '🔒';
        left.appendChild(lock);
      }
      const right = document.createElement('span');
      right.className = 'wifi-meta';
      const rssi = document.createElement('span');
      rssi.className = 'wifi-rssi ' + rssiClass(n.rssi);
      rssi.textContent = rssiLabel(n.rssi) + ' (' + n.rssi + 'dBm)';
      right.appendChild(rssi);
      item.appendChild(left);
      item.appendChild(right);
      item.addEventListener('click', () => {
        $('wifi-ssid').value = n.ssid;
        $('wifi-password').value = '';
        $('wifi-password').focus();
        list.querySelectorAll('.wifi-item').forEach(i => i.classList.remove('selected'));
        item.classList.add('selected');
      });
      list.appendChild(item);
    });
  } catch (e) {
    $('wifi-scan-status').textContent = '扫描失败: ' + e.message;
  } finally {
    $('wifi-scan-btn').disabled = false;
  }
}

$('wifi-scan-btn').addEventListener('click', scanWifi);

$('wifi-connect-btn').addEventListener('click', async () => {
  const ssid = $('wifi-ssid').value.trim();
  const pwd = $('wifi-password').value;
  if (!ssid) {
    alert('请输入或选择 SSID');
    return;
  }
  $('wifi-connect-btn').disabled = true;
  $('wifi-connect-status').textContent = '连接中...（ESP8266 切换到 STA 模式，连接成功后请切换到新 WiFi 访问）';
  try {
    const resp = await fetch('/api/wifi/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid: ssid, password: pwd })
    });
    const result = await resp.json();
    if (result.success) {
      // 响应只是"开始连接"，真正的结果通过 SSE 或下一次状态查询得知
      $('wifi-connect-status').textContent = '正在连接，等待结果...';
      // 等 10 秒后查询状态（如果 ESP8266 已切换 STA，AP 会断开，请求会失败）
      setTimeout(async () => {
        try {
          const sr = await fetch('/api/status');
          const s = await sr.json();
          if (s.wifi && s.wifi.mode === 'sta' && s.wifi.ip && s.wifi.ip !== '0.0.0.0') {
            $('wifi-connect-status').innerHTML =
              '<strong style="color: var(--success)">连接成功！</strong><br>' +
              '新 IP: ' + s.wifi.ip + '<br>' +
              '请切换到 ' + ssid + ' WiFi 后访问 http://' + s.wifi.ip + '/';
          } else {
            $('wifi-connect-status').textContent = '连接失败，请检查密码或信号';
          }
        } catch (e) {
          // ESP8266 已切换 STA，AP 断开，无法访问
          $('wifi-connect-status').innerHTML =
            'ESP8266 已切换网络模式。请将电脑/手机切回 <strong>' + ssid +
            '</strong> WiFi，然后通过串口监视器查看新 IP，或访问路由器后台查找。';
        }
      }, 10000);
    } else {
      $('wifi-connect-status').textContent = '启动失败: ' + (result.error || '未知错误');
      $('wifi-connect-btn').disabled = false;
    }
  } catch (e) {
    $('wifi-connect-status').textContent = '请求失败: ' + e.message;
    $('wifi-connect-btn').disabled = false;
  }
});

$('save-monitor-btn').addEventListener('click', async () => {
  const cfg = { monitor_baud: parseInt($('default-baud-select').value) };
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(cfg)
  });
  const r = await resp.json();
  if (r.success) {
    $('baud-select').value = cfg.monitor_baud;
    alert('已保存');
  } else {
    alert('保存失败: ' + r.error);
  }
});

$('save-burn-btn').addEventListener('click', async () => {
  const cfg = { verify: $('default-verify-check').checked };
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(cfg)
  });
  const r = await resp.json();
  if (r.success) {
    $('verify-check').checked = cfg.verify;
    alert('已保存');
  } else {
    alert('保存失败: ' + r.error);
  }
});

$('reboot-btn').addEventListener('click', async () => {
  if (!confirm('确定重启 ESP8266?')) return;
  try {
    await fetch('/api/reboot', { method: 'POST' });
    alert('已发送重启指令，等待重新连接...');
  } catch (e) {
    alert('请求失败: ' + e.message);
  }
});

// ===== 状态轮询 =====
async function pollStatus() {
  try {
    const resp = await fetch('/api/status');
    const s = await resp.json();
    setStateBadge(s.state);
    if (s.wifi) {
      $('ip-info').textContent = 'IP: ' + s.wifi.ip;
      $('device-ip').textContent = s.wifi.ip;
      $('rssi').textContent = s.wifi.rssi;
    }
    // 配网模式提示
    if (s.config_mode !== undefined) {
      const hint = $('wifi-mode-hint');
      if (s.config_mode) {
        hint.innerHTML = '<strong style="color: var(--warning)">配网模式</strong>：ESP8266 开放 AP "CCLoader-Setup"，请保持电脑/手机连此 AP 完成配网';
      } else if (s.wifi && s.wifi.mode === 'sta') {
        hint.innerHTML = '<strong style="color: var(--success)">STA 模式</strong>：已连接 ' + (s.wifi.ssid || '') + '，IP ' + (s.wifi.ip || '-');
      } else {
        hint.textContent = '';
      }
    }
    if (s.uptime !== undefined) {
      const h = Math.floor(s.uptime / 3600);
      const m = Math.floor((s.uptime % 3600) / 60);
      $('uptime').textContent = h + '时' + m + '分';
    }
    if (s.monitor) {
      if (s.monitor.active && !monitorActive) {
        onMonitorStart(s.monitor.baud);
      } else if (!s.monitor.active && monitorActive) {
        onMonitorStop();
      }
    }
  } catch (e) {}
}

// ===== 初始化 =====
function init() {
  connectSSE();
  refreshFileList();
  loadConfig();
  pollStatus();
  setInterval(pollStatus, 3000);
}

init();
