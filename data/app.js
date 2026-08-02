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
  } else if (state === 'sniffing') {
    badge.classList.add('sniffing');
    badge.textContent = '抓包中';
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
      case 'sniffer_start':
        onSnifferStart(msg.channel);
        break;
      case 'sniffer_stop':
        onSnifferStop();
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
          ' 密码错误或信号太弱，ESP32-SOLO-1 已切回 AP 模式';
        $('wifi-connect-btn').disabled = false;
        break;
      case 'status':
        if (msg.state) setStateBadge(msg.state);
        break;
    }
  };

  es.onerror = () => {
    $('ws-state').textContent = '已断开';
    // EventSource 会自动重连，但 ESP32-SOLO-1 断电后需手动兜底
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
      // time=0 表示未授时（AP 配网模式无 NTP），显示"刚刚"而非 1970
      const date = (f.time > 0) ? new Date(f.time * 1000).toLocaleString('zh-CN') : '刚刚';

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
      delBtn.dataset.name = f.name;
      delBtn.textContent = '删除';

      const dlBtn = document.createElement('button');
      dlBtn.className = 'btn download-btn';
      dlBtn.dataset.name = f.name;
      dlBtn.textContent = '下载';

      const btnGroup = document.createElement('div');
      btnGroup.className = 'file-btn-group';
      btnGroup.appendChild(dlBtn);
      btnGroup.appendChild(delBtn);

      item.appendChild(info);
      item.appendChild(btnGroup);
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
    // 下载事件
    list.querySelectorAll('.download-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const url = '/api/files/' + encodeURIComponent(btn.dataset.name);
        const a = document.createElement('a');
        a.href = url;
        a.download = btn.dataset.name;
        a.click();
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
  $('burn-btn').disabled = true;
  $('burn-log').innerHTML = '';
  $('burn-progress-bar').style.width = '0%';
  $('burn-progress-text').textContent = '0%';
  appendLog($('burn-log'), '开始烧录: ' + selectedFile, 'success');

  try {
    const resp = await fetch('/api/burn', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ filename: selectedFile })
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

$('nvreset-btn').addEventListener('click', async () => {
  if (!confirm('确定清除 CC2530 的配网信息？\n\n固件不会被擦除，但设备需要重新加入 Zigbee 网络。\n此操作不可撤销！')) return;
  $('nvreset-btn').disabled = true;
  $('burn-btn').disabled = true;
  $('burn-log').innerHTML = '';
  $('burn-progress-bar').style.width = '0%';
  $('burn-progress-text').textContent = '0%';
  appendLog($('burn-log'), '开始清除配网：读取 Flash → 清除 NV → 写回', 'success');
  try {
    const resp = await fetch('/api/nvreset', { method: 'POST' });
    const result = await resp.json();
    if (!result.success) {
      appendLog($('burn-log'), '启动失败: ' + result.error, 'error');
      $('nvreset-btn').disabled = false;
      $('burn-btn').disabled = false;
    }
  } catch (e) {
    appendLog($('burn-log'), '请求失败: ' + e.message, 'error');
    $('nvreset-btn').disabled = false;
    $('burn-btn').disabled = false;
  }
});

$('backup-btn').addEventListener('click', async () => {
  if (!confirm('确定备份 CC2530 固件？\n\n读取 Flash 保存到烧录器，约 1 分钟。\n完成后可通过文件列表下载到本地。')) return;
  $('backup-btn').disabled = true;
  $('burn-btn').disabled = true;
  $('burn-log').innerHTML = '';
  $('burn-progress-bar').style.width = '0%';
  $('burn-progress-text').textContent = '0%';
  appendLog($('burn-log'), '开始备份固件：读取 Flash 保存到 LittleFS...', 'success');
  try {
    const resp = await fetch('/api/backup', { method: 'POST' });
    const result = await resp.json();
    if (!result.success) {
      appendLog($('burn-log'), '启动失败: ' + result.error, 'error');
      $('backup-btn').disabled = false;
      $('burn-btn').disabled = false;
    }
  } catch (e) {
    appendLog($('burn-log'), '请求失败: ' + e.message, 'error');
    $('backup-btn').disabled = false;
    $('burn-btn').disabled = false;
  }
});

function updateBurnProgress(msg) {
  $('burn-progress-bar').style.width = msg.percent + '%';
  $('burn-progress-text').textContent = msg.percent + '% (' + msg.current_block + '/' + msg.total_blocks + ')';
  if (msg.current_block > 0 && msg.current_block % 50 === 0 && msg.current_block < msg.total_blocks) {
    appendLog($('burn-log'), '处理块 ' + msg.current_block + '/' + msg.total_blocks);
  }
  if (msg.error) {
    appendLog($('burn-log'), '错误: ' + msg.error, 'error');
  }
  if (msg.info) {
    appendLog($('burn-log'), msg.info, 'success');
  }
  if (msg.done) {
    appendLog($('burn-log'), '操作完成', 'success');
    $('burn-btn').disabled = false;
    $('nvreset-btn').disabled = false;
    $('backup-btn').disabled = false;
    // 备份完成后刷新文件列表
    if (msg.info) refreshFileList();
    if ($('reboot-burner-check').checked) {
      appendLog($('burn-log'), '3 秒后重启烧录器...', 'success');
      setTimeout(() => {
        appendLog($('burn-log'), '正在重启烧录器...', 'success');
        fetch('/api/reboot', { method: 'POST' }).catch(() => {});
      }, 3000);
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
      const err = result.error || '未知错误';
      const msg = err === 'busy' ? '设备忙（烧录中或监控中），请先停止当前操作' : '启动监控失败: ' + err;
      alert(msg);
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
  setStateBadge('monitoring');  // 立即更新顶部徽章
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
    appendLog($('monitor-log'), '监控开始 @ ' + baud + ' bps（已自动复位 CC2530，捕获启动日志）', 'success');
  } else {
    appendLog($('monitor-log'), '监控开始 @ ' + baud + ' bps（非侵入式），点"复位 CC2530"可重启目标', 'success');
  }
}

function onMonitorStop() {
  monitorActive = false;
  setStateBadge('idle');  // 立即更新顶部徽章
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
    $('device-name-input').value = cfg.device_name || '';
    // 同步浏览器标签页标题
    updateDocumentTitle(cfg.device_name);
  } catch (e) {
    console.error('loadConfig error:', e);
  }
}

function updateDocumentTitle(name) {
  const title = (name && name.trim()) ? name.trim() : 'CCLoader WebUI';
  document.title = title;
  const headerTitle = document.getElementById('header-title');
  if (headerTitle) headerTitle.textContent = title;
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
  $('wifi-connect-status').textContent = '连接中...（ESP32-SOLO-1 切换到 STA 模式，连接成功后请切换到新 WiFi 访问）';
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
      // 等 10 秒后查询状态（如果 ESP32-SOLO-1 已切换 STA，AP 会断开，请求会失败）
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
          // ESP32-SOLO-1 已切换 STA，AP 断开，无法访问
          $('wifi-connect-status').innerHTML =
            'ESP32-SOLO-1 已切换网络模式。请将电脑/手机切回 <strong>' + ssid +
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

$('save-device-name-btn').addEventListener('click', async () => {
  const name = $('device-name-input').value.trim();
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ device_name: name })
  });
  const r = await resp.json();
  if (r.success) {
    updateDocumentTitle(name);
    alert('已保存');
  } else {
    alert('保存失败: ' + r.error);
  }
});

$('reboot-btn').addEventListener('click', async () => {
  if (!confirm('确定重启 ESP32-SOLO-1?')) return;
  try {
    await fetch('/api/reboot', { method: 'POST' });
    alert('已发送重启指令，等待重新连接...');
  } catch (e) {
    alert('请求失败: ' + e.message);
  }
});

// ===== 状态轮询 =====
async function pollStatus() {
  // sniffer stream 期间 handleSnifferStream 阻塞 WebServer（单线程），
  // fetch('/api/status') 会超时失败，跳过避免无效请求
  if (snifferActive && snifferStreamController) return;
  try {
    const resp = await fetch('/api/status');
    const s = await resp.json();
    setStateBadge(s.state);
    if (s.wifi) {
      $('ip-info').textContent = 'IP: ' + s.wifi.ip;
      $('device-ip').textContent = s.wifi.ip;
      $('rssi').textContent = s.wifi.rssi !== undefined ? s.wifi.rssi + ' dBm' : '-';
    }
    // 配网模式提示
    if (s.config_mode !== undefined) {
      const hint = $('wifi-mode-hint');
      if (s.config_mode) {
        const apName = (s.hardware && s.hardware.ap_name) || 'CCLoader-XXXXXX';
        hint.innerHTML = '<strong style="color: var(--warning)">配网模式</strong>：开放 AP "' + apName + '"，请保持电脑/手机连此 AP 完成配网';
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
    // Flash 资源占用（app 分区，非物理 Flash 芯片大小）
    if (s.flash) {
      const used = (s.flash.sketch_size / 1024).toFixed(0);
      const total = ((s.flash.sketch_size + s.flash.sketch_free) / 1024).toFixed(0);
      const pct = (s.flash.sketch_size / (s.flash.sketch_size + s.flash.sketch_free) * 100).toFixed(1);
      $('flash-usage').textContent = used + 'KB / ' + total + 'KB (' + pct + '%)';
    }
    // 版本号 + 编译日期（从后端同步）
    if (s.version) {
      $('firmware-version').textContent = s.version;
    }
    if (s.build_time) {
      // 编译日期格式化：Jul 30 2026 22:19:44 -> 2026-07-30 22:19:44
      const d = new Date(s.build_time);
      if (!isNaN(d.getTime())) {
        const pad = n => String(n).padStart(2, '0');
        $('build-time').textContent = d.getFullYear() + '-' + pad(d.getMonth() + 1) +
                                      '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' +
                                      pad(d.getMinutes()) + ':' + pad(d.getSeconds());
      } else {
        $('build-time').textContent = s.build_time;  // 解析失败原样显示
      }
    }
    // 复位原因
    if (s.reset_reason) {
      $('reset-reason').textContent = s.reset_reason;
    }
    // 内存信息
    if (s.memory) {
      const freeKb = (s.memory.free_heap / 1024).toFixed(1);
      $('free-heap').textContent = freeKb + ' KB';
      const usedKb = (s.memory.ram_used / 1024).toFixed(1);
      const totalKb = (s.memory.ram_size / 1024).toFixed(0);
      $('ram-usage').textContent = usedKb + ' KB / ' + totalKb + ' KB (' + s.memory.ram_pct + '%)';
    } else if (s.free_heap !== undefined) {
      // 兼容旧固件：free_heap 在顶层
      $('free-heap').textContent = (s.free_heap / 1024).toFixed(1) + ' KB';
    }
    // 硬件信息（新增）
    if (s.hardware) {
      $('chip-model').textContent = s.hardware.chip_model || '-';
      $('chip-revision').textContent = 'rev ' + (s.hardware.chip_revision !== undefined ? s.hardware.chip_revision : '-');
      $('cpu-freq').textContent = s.hardware.cpu_freq !== undefined ? s.hardware.cpu_freq + ' MHz' : '-';
      // Flash 芯片：物理芯片大小（如 4MB），与 app 分区（如 2MB）区分
      // app 分区大小 = sketch_size + sketch_free，在"内存信息 > 应用分区"中显示
      $('flash-size-info').textContent = s.hardware.flash_size ? (s.hardware.flash_size / 1024 / 1024).toFixed(0) + ' MB' : '-';
      $('mac-info').textContent = s.hardware.mac || '-';
      $('hostname-info').textContent = s.hardware.hostname || '-';
      $('ap-name-info').textContent = s.hardware.ap_name || '-';
      $('sdk-version').textContent = s.hardware.sdk_version || '-';
    }
    // 设备名称（同步 header 标题和标签页标题）
    if (s.device_name !== undefined) {
      updateDocumentTitle(s.device_name);
    }
    if (s.monitor) {
      if (s.monitor.active && !monitorActive) {
        onMonitorStart(s.monitor.baud);
      } else if (!s.monitor.active && monitorActive) {
        onMonitorStop();
      }
    }
    if (s.sniffer) {
      if (s.sniffer.active && !snifferActive) {
        onSnifferStart(s.sniffer.channel);
      } else if (!s.sniffer.active && snifferActive) {
        onSnifferStop();
      }
      // 同步丢包统计（stream 期间无法调用 status，这里补偿更新）
      if (s.sniffer.active) {
        $('sniffer-drop').textContent = s.sniffer.dropped_bytes || 0;
      }
    }
  } catch (e) {}
}

// ===== 帮助页：从 /api/help 加载 markdown，用 marked.js（CDN）渲染 =====
let helpRawText = '';  // 保留原文用于一键复制
async function loadHelp() {
  try {
    const resp = await fetch('/api/help');
    const text = await resp.text();
    helpRawText = text;
    // marked.js 从 CDN 加载（index.html 末尾引入）。若 CDN 加载失败则降级显示原文
    if (typeof marked !== 'undefined' && marked.parse) {
      $('help-content').innerHTML = marked.parse(text);
    } else {
      $('help-content').innerHTML = '<pre style="white-space:pre-wrap;">' +
        text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;') + '</pre>';
    }
  } catch (e) {
    $('help-content').textContent = '加载失败: ' + e.message;
  }
}

$('help-copy-btn').addEventListener('click', async () => {
  if (!helpRawText) {
    alert('帮助内容尚未加载');
    return;
  }
  try {
    await navigator.clipboard.writeText(helpRawText);
    alert('已复制帮助原文到剪贴板');
  } catch (e) {
    // 降级方案：选中文本
    const range = document.createRange();
    range.selectNode($('help-content'));
    window.getSelection().removeAllRanges();
    window.getSelection().addRange(range);
    alert('剪贴板不可用，已选中文本，请手动 Ctrl+C 复制');
  }
});

// ===== Sniffer 抓包 =====
// ZBOSS 协议：包头 4 字节（len, type, tail[2]）+ payload（len-4 字节）
// type=0x00 OK / 0x01 TOO_BIG / 0x02 OVERFLOW / 0xFF 丢包标记
// payload 最后 1 字节：CRC 状态（bit7=1 OK, bit7=0 BAD）
let snifferActive = false;
let snifferStreamController = null;  // AbortController
let snifferPackets = [];  // 已解析的 IEEE 帧（用于 pcap 下载）
let snifferPktCount = 0;
let snifferRxBytes = 0;
let snifferChannel = 11;
let snifferDetailMode = false;
let snifferStreamBuf = new Uint8Array(0);  // 流式接收缓冲

function onSnifferStart(channel) {
  if (snifferActive) return;  // 避免重复触发（SSE + pollStatus）
  snifferActive = true;
  setStateBadge('sniffing');  // 立即更新顶部徽章（stream 期间 WebServer 阻塞，轮询失效）
  snifferChannel = channel;
  snifferPktCount = 0;
  snifferRxBytes = 0;
  snifferPackets = [];
  snifferStreamBuf = new Uint8Array(0);
  $('sniffer-pkt-tbody').innerHTML = '';
  $('sniffer-pkt-count').textContent = '0';
  $('sniffer-rx').textContent = '0';
  $('sniffer-drop').textContent = '0';
  $('sniffer-state').textContent = '抓包中 @ CH' + channel;
  $('sniffer-start-btn').disabled = true;
  $('sniffer-stop-btn').disabled = false;
  $('sniffer-channel-switch-btn').disabled = false;
  $('sniffer-clear-btn').disabled = false;
  $('sniffer-download-btn').disabled = false;
  // 开始流式接收（SSE 触发，此时 sniffer 已启动）
  readSnifferStream();
}

function onSnifferStop() {
  if (!snifferActive) return;
  snifferActive = false;
  setStateBadge('idle');  // 立即更新顶部徽章
  if (snifferStreamController) {
    snifferStreamController.abort();
    snifferStreamController = null;
  }
  $('sniffer-state').textContent = '已停止';
  $('sniffer-start-btn').disabled = false;
  $('sniffer-stop-btn').disabled = true;
  $('sniffer-channel-switch-btn').disabled = true;
}

async function readSnifferStream() {
  if (snifferStreamController) return;  // 已有 stream 在读
  snifferStreamController = new AbortController();
  try {
    const resp = await fetch('/api/sniffer/stream', { signal: snifferStreamController.signal });
    if (!resp.ok) {
      $('sniffer-state').textContent = 'stream 错误: ' + resp.status;
      snifferStreamController = null;
      return;
    }
    const reader = resp.body.getReader();
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      // 追加到缓冲
      const newBuf = new Uint8Array(snifferStreamBuf.length + value.length);
      newBuf.set(snifferStreamBuf);
      newBuf.set(value, snifferStreamBuf.length);
      snifferStreamBuf = newBuf;
      snifferRxBytes += value.length;
      $('sniffer-rx').textContent = snifferRxBytes;
      // 解析 ZBOSS 包
      parseZbossPackets();
    }
  } catch (e) {
    if (e.name !== 'AbortError') {
      console.error('Sniffer stream error:', e);
    }
  }
  snifferStreamController = null;
}

function parseZbossPackets() {
  let pos = 0;
  while (pos + 4 <= snifferStreamBuf.length) {
    const len = snifferStreamBuf[pos];
    // len < 5（4 字节包头 + 至少 1 字节 payload）非法；len > 127 超出 ZBOSS 协议
    if (len < 5 || len > 127) {
      pos++;
      continue;
    }
    if (pos + len > snifferStreamBuf.length) break;  // 数据不完整，等下次
    const type = snifferStreamBuf[pos + 1];
    const payload = snifferStreamBuf.slice(pos + 4, pos + len);
    handleZbossPacket(type, payload);
    pos += len;
  }
  if (pos > 0) {
    snifferStreamBuf = snifferStreamBuf.slice(pos);
  }
}

function handleZbossPacket(type, payload) {
  snifferPktCount++;
  let typeStr, crcStr, hexPreview, detail = '';
  let ieeeFrame = null;

  if (type === 0xFF) {
    // 丢包标记：payload 是 4 字节大端 dropped_bytes
    const dropped = (payload.length >= 4)
      ? ((payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3]) >>> 0
      : 0;
    typeStr = '<span class="pkt-type-drop">DROP</span>';
    crcStr = '-';
    hexPreview = '丢包 ' + dropped + ' B';
  } else if (type === 0x00) {
    typeStr = '<span class="pkt-type-ok">OK</span>';
    // payload 最后 1 字节是 CRC 状态
    const crcByte = payload.length > 0 ? payload[payload.length - 1] : 0;
    crcStr = (crcByte & 0x80) ? '<span class="pkt-crc-ok">OK</span>' : '<span class="pkt-crc-bad">BAD</span>';
    // IEEE 802.15.4 帧（移除最后 1 字节 CRC 状态）
    ieeeFrame = payload.slice(0, payload.length - 1);
    hexPreview = bytesToHex(ieeeFrame, 32);
    if (snifferDetailMode) {
      detail = parseIeee802154Frame(ieeeFrame);
    }
    snifferPackets.push(ieeeFrame);
  } else if (type === 0x01) {
    typeStr = '<span class="pkt-type-err">TOO_BIG</span>';
    crcStr = '-';
    hexPreview = bytesToHex(payload, 32);
  } else if (type === 0x02) {
    typeStr = '<span class="pkt-type-err">OVF</span>';
    crcStr = '-';
    hexPreview = bytesToHex(payload, 32);
  } else {
    typeStr = '<span class="pkt-type-err">0x' + type.toString(16) + '</span>';
    crcStr = '-';
    hexPreview = bytesToHex(payload, 32);
  }

  appendPacketRow(snifferPktCount, snifferChannel, payload.length, typeStr, crcStr, hexPreview, detail);
  $('sniffer-pkt-count').textContent = snifferPktCount;
}

function bytesToHex(bytes, maxLen) {
  let hex = '';
  const n = Math.min(bytes.length, maxLen);
  for (let i = 0; i < n; i++) {
    hex += bytes[i].toString(16).padStart(2, '0') + ' ';
  }
  if (bytes.length > maxLen) hex += '...';
  return hex.trim();
}

// IEEE 802.15.4 帧头解析（详细解析模式）
function parseIeee802154Frame(frame) {
  if (frame.length < 3) return '';
  const fc = frame[0] | (frame[1] << 8);
  const frameType = fc & 0x07;
  const dstAddrMode = (fc >> 10) & 0x03;
  const srcAddrMode = (fc >> 14) & 0x03;
  const panIdComp = (fc >> 6) & 0x01;
  const secEnabled = (fc >> 3) & 0x01;
  const ackReq = (fc >> 5) & 0x01;

  const typeNames = ['Beacon', 'Data', 'Ack', 'MAC_Cmd'];
  let detail = typeNames[frameType] || 'Type' + frameType;
  if (secEnabled) detail += ' [SEC]';
  if (ackReq) detail += ' [AR]';

  // Ack 帧只有 2 字节 FC + 1 字节 Seq
  if (frameType === 2) {
    return detail + ' seq=' + frame[2].toString(16).padStart(2, '0');
  }

  let pos = 3;  // FC(2) + Seq(1)

  // 目的地址
  if (dstAddrMode !== 0 && pos + 2 <= frame.length) {
    const dstPan = frame[pos] | (frame[pos + 1] << 8);
    pos += 2;
    detail += ' dPAN=' + dstPan.toString(16).padStart(4, '0');
    if (dstAddrMode === 2 && pos + 2 <= frame.length) {
      const dstAddr = frame[pos] | (frame[pos + 1] << 8);
      pos += 2;
      detail += ' dst=' + dstAddr.toString(16).padStart(4, '0');
    } else if (dstAddrMode === 3 && pos + 8 <= frame.length) {
      let dstAddr = '';
      for (let i = 7; i >= 0; i--) dstAddr += frame[pos + i].toString(16).padStart(2, '0');
      pos += 8;
      detail += ' dst=' + dstAddr;
    }
  }

  // 源地址
  if (srcAddrMode !== 0) {
    if (panIdComp === 0 && pos + 2 <= frame.length) {
      const srcPan = frame[pos] | (frame[pos + 1] << 8);
      pos += 2;
      detail += ' sPAN=' + srcPan.toString(16).padStart(4, '0');
    }
    if (srcAddrMode === 2 && pos + 2 <= frame.length) {
      const srcAddr = frame[pos] | (frame[pos + 1] << 8);
      pos += 2;
      detail += ' src=' + srcAddr.toString(16).padStart(4, '0');
    } else if (srcAddrMode === 3 && pos + 8 <= frame.length) {
      let srcAddr = '';
      for (let i = 7; i >= 0; i--) srcAddr += frame[pos + i].toString(16).padStart(2, '0');
      pos += 8;
      detail += ' src=' + srcAddr;
    }
  }

  return detail;
}

function appendPacketRow(idx, ch, len, typeStr, crcStr, hexPreview, detail) {
  const tbody = $('sniffer-pkt-tbody');
  const tr = document.createElement('tr');
  const now = new Date();
  const timeStr = pad(now.getHours(), 2) + ':' + pad(now.getMinutes(), 2) + ':' +
                  pad(now.getSeconds(), 2) + '.' + pad(now.getMilliseconds(), 3);
  tr.innerHTML = '<td class="col-idx">' + idx + '</td>' +
    '<td class="col-time">' + timeStr + '</td>' +
    '<td class="col-ch">' + ch + '</td>' +
    '<td class="col-len">' + len + '</td>' +
    '<td class="col-type">' + typeStr + '</td>' +
    '<td class="col-crc">' + crcStr + '</td>' +
    '<td class="col-hex">' + hexPreview + '</td>' +
    '<td class="col-detail">' + detail + '</td>';
  tbody.appendChild(tr);
  // 限制 2000 行避免内存溢出
  while (tbody.children.length > 2000) {
    tbody.removeChild(tbody.firstChild);
    // 同步移除 snifferPackets（保持索引一致）
    if (snifferPackets.length > 0) snifferPackets.shift();
  }
  // 自动滚动到底部
  const wrap = tbody.parentElement.parentElement;
  wrap.scrollTop = wrap.scrollHeight;
}

function clearSnifferPackets() {
  snifferPackets = [];
  snifferPktCount = 0;
  $('sniffer-pkt-tbody').innerHTML = '';
  $('sniffer-pkt-count').textContent = '0';
}

function downloadPcap() {
  // pcap 全局头（24 字节，DLT_IEEE802_15_4_NOFCS=230）
  const header = new Uint8Array(24);
  const dv = new DataView(header.buffer);
  dv.setUint32(0, 0xa1b2c3d4, true);   // magic（小端）
  dv.setUint16(4, 2, true);            // version_major
  dv.setUint16(6, 4, true);            // version_minor
  dv.setInt32(8, 0, true);             // thiszone
  dv.setUint32(12, 0, true);           // sigfigs
  dv.setUint32(16, 65535, true);       // snaplen
  dv.setUint32(20, 230, true);         // dlt

  const parts = [header];
  const baseSec = Math.floor(Date.now() / 1000);
  let usec = 0;
  for (const pkt of snifferPackets) {
    const recHeader = new Uint8Array(16);
    const recDv = new DataView(recHeader.buffer);
    recDv.setUint32(0, baseSec, true);           // ts_sec
    recDv.setUint32(4, usec, true);              // ts_usec
    recDv.setUint32(8, pkt.length, true);        // caplen
    recDv.setUint32(12, pkt.length, true);       // origlen
    parts.push(recHeader);
    parts.push(pkt);
    usec = (usec + 1000) % 1000000;  // 模拟时间递增
  }

  const blob = new Blob(parts, { type: 'application/octet-stream' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'capture_' + Date.now() + '.pcap';
  a.click();
  URL.revokeObjectURL(a.href);
}

$('sniffer-start-btn').addEventListener('click', async () => {
  const channel = parseInt($('sniffer-channel-select').value);
  $('sniffer-start-btn').disabled = true;
  try {
    const resp = await fetch('/api/sniffer/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ channel: channel, baud: 115200 })
    });
    const data = await resp.json();
    if (!resp.ok) {
      alert('启动失败: ' + (data.error || resp.status));
      $('sniffer-start-btn').disabled = false;
      return;
    }
    // POST 成功后立即更新状态并启动 stream，不等 SSE sniffer_start 事件
    // 原因：handleSnifferStream 阻塞 WebServer（单线程），SSE 事件可能延迟到达
    // onSnifferStart 内有 if (snifferActive) return; 防止 SSE 事件重复触发
    onSnifferStart(channel);
  } catch (e) {
    alert('启动失败: ' + e.message);
    $('sniffer-start-btn').disabled = false;
  }
});

$('sniffer-stop-btn').addEventListener('click', async () => {
  // 先 abort stream，否则 /api/stop 无法响应（HTTP 单线程阻塞）
  if (snifferStreamController) {
    snifferStreamController.abort();
    snifferStreamController = null;
  }
  try {
    await fetch('/api/stop', { method: 'POST' });
  } catch (e) {}
  onSnifferStop();
});

$('sniffer-channel-switch-btn').addEventListener('click', async () => {
  const channel = parseInt($('sniffer-channel-select').value);
  if (channel === snifferChannel) {
    alert('当前已是通道 ' + channel);
    return;
  }
  // 先 abort stream，否则 /api/sniffer/channel 无法响应
  if (snifferStreamController) {
    snifferStreamController.abort();
    snifferStreamController = null;
  }
  try {
    const resp = await fetch('/api/sniffer/channel', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ channel: channel })
    });
    const data = await resp.json();
    if (!resp.ok) {
      alert('切换通道失败: ' + (data.error || data.message || resp.status));
      return;
    }
    snifferChannel = channel;
    $('sniffer-state').textContent = '抓包中 @ CH' + channel;
    // 清空流缓冲（避免残留数据污染新通道）
    snifferStreamBuf = new Uint8Array(0);
    // 重新连接 stream
    readSnifferStream();
  } catch (e) {
    alert('切换通道失败: ' + e.message);
  }
});

$('sniffer-clear-btn').addEventListener('click', () => {
  clearSnifferPackets();
});

$('sniffer-download-btn').addEventListener('click', () => {
  if (snifferPackets.length === 0) {
    alert('没有可下载的包');
    return;
  }
  downloadPcap();
});

$('sniffer-detail-check').addEventListener('change', (e) => {
  snifferDetailMode = e.target.checked;
});

// ===== 初始化 =====
function init() {
  connectSSE();
  refreshFileList();
  loadConfig();
  loadHelp();
  pollStatus();
  setInterval(pollStatus, 3000);
}

init();
