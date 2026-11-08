/* E252 EtherCAT 主站调试台 —— 前端逻辑（无构建步骤） */

const $ = (id) => document.getElementById(id);
const APP = {
  snap: null,
  pos: null,
  detail: null,
  mapping: null,
  od: null,
  selectedObject: null,
  tab: 'overview',
  paused: false,
  busy: false,
  adapters: [],
};

/* ---------------- 基础工具 ---------------- */
async function api(path, options = {}) {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...options,
  });
  let data = null;
  try { data = await res.json(); } catch (e) { data = { ok: false, error: `HTTP ${res.status}` }; }
  if (!res.ok || data.ok === false) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

let toastTimer = null;
function toast(message, kind = 'err') {
  const el = $('toast');
  el.textContent = message;
  el.className = `toast ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.add('hidden'), kind === 'err' ? 9000 : 3500);
}

function hex(v, width = 2) {
  return '0x' + Number(v).toString(16).toUpperCase().padStart(width, '0');
}
function bytesHex(arr, sep = ' ') {
  if (!arr || !arr.length) return '—';
  return arr.map((b) => (b & 0xFF).toString(16).padStart(2, '0')).join(sep);
}
function parseIndex(text) {
  const t = String(text || '').trim();
  if (!t) throw new Error('请输入索引');
  return t.toLowerCase().startsWith('0x') ? parseInt(t, 16) : parseInt(t, 10);
}
function stateClass(name) {
  const n = (name || '').toUpperCase();
  if (n.includes('ERR')) return 'err';
  if (n === 'OP') return 'op';
  if (n === 'SAFEOP') return 'safeop';
  if (n === 'PREOP') return 'preop';
  return 'init';
}
function selectedSlave() {
  if (!APP.snap || !APP.snap.slaves) return null;
  return APP.snap.slaves.find((s) => s.pos === APP.pos) || null;
}
function esc(text) {
  return String(text == null ? '' : text).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

/* ---------------- 网卡与连接 ---------------- */
async function loadAdapters() {
  try {
    const data = await api('/api/adapters');
    APP.adapters = data.adapters || [];
    const sel = $('adapterSelect');
    sel.innerHTML = '';
    APP.adapters.forEach((a) => {
      const opt = document.createElement('option');
      opt.value = a.name;
      opt.textContent = `${a.desc}  ·  ${a.name}`;
      sel.appendChild(opt);
    });
    const preferred = APP.adapters.find((a) => /EtherCAT|USB/i.test(a.desc)) || APP.adapters.find((a) => !/WAN Miniport|loopback|Wi-Fi Direct/i.test(a.desc));
    if (preferred) sel.value = preferred.name;
  } catch (e) {
    toast(`读取网卡失败：${e.message}`);
  }
}

async function connect() {
  const sel = $('adapterSelect');
  if (!sel.value) { toast('请选择网卡'); return; }
  const desc = (APP.adapters.find((a) => a.name === sel.value) || {}).desc || '';
  setBusy(true);
  try {
    const data = await api('/api/connect', { method: 'POST', body: JSON.stringify({ adapter: sel.value, desc }) });
    APP.snap = data;
    if (data.slaves && data.slaves.length) selectSlave(data.slaves[0].pos);
    toast('主站已连接，周期通信已启动', 'ok');
  } catch (e) {
    toast(`连接失败：${e.message}`);
  } finally {
    setBusy(false);
  }
}

async function disconnect() {
  setBusy(true);
  try {
    await api('/api/disconnect', { method: 'POST', body: '{}' });
    APP.pos = null; APP.detail = null; APP.od = null; APP.mapping = null;
    toast('已断开', 'ok');
  } catch (e) {
    toast(e.message);
  } finally {
    setBusy(false);
  }
}

function setBusy(busy) {
  APP.busy = busy;
  const connected = !!(APP.snap && APP.snap.connected);
  $('btnAck').disabled = busy || !connected;
  $('btnConnect').disabled = busy || connected;
  $('btnDisconnect').disabled = busy || !connected;
  document.querySelectorAll('.state-btn').forEach((b) => { b.disabled = busy || !connected; });
  document.querySelectorAll('.demo-cmd').forEach((b) => { b.disabled = busy || !connected; });
}

/* ---------------- 状态机 ---------------- */
async function requestState(target) {
  setBusy(true);
  try {
    await api('/api/state', { method: 'POST', body: JSON.stringify({ state: target, timeout: 5 }) });
    toast(`已进入 ${target}`, 'ok');
  } catch (e) {
    toast(e.message);
  } finally {
    setBusy(false);
  }
}

async function ackState() {
  setBusy(true);
  try {
    const res = await api('/api/state/ack', { method: 'POST', body: JSON.stringify({}) });
    const after = (res.after || []).map((d) => `S${d.pos}: ${stateNameOf(d.state)} / AL=0x${d.al.toString(16).toUpperCase().padStart(4, '0')}`).join('，');
    toast(`ACK 完成：${after}`, 'ok');
  } catch (e) {
    toast(e.message);
  } finally {
    setBusy(false);
  }
}

/* ---------------- 从站选择 ---------------- */
async function selectSlave(pos) {
  APP.pos = pos;
  document.querySelectorAll('.slave-card').forEach((c) => c.classList.toggle('selected', Number(c.dataset.pos) === pos));
  try {
    const [detail, mapping, od] = await Promise.all([
      api(`/api/slaves/${pos}`),
      api(`/api/slaves/${pos}/mapping`).catch(() => null),
      api(`/api/slaves/${pos}/od`).catch(() => null),
    ]);
    APP.detail = detail;
    APP.mapping = mapping;
    APP.od = od;
    APP.selectedObject = null;
    renderOdList();
    buildPdoRows();
    if (APP.tab === 'overview') renderOverview();
    if (APP.tab === 'demo') renderDemo();
    if (APP.tab === 'pdo') renderPdoLive();
  } catch (e) {
    toast(`读取从站信息失败：${e.message}`);
  }
}

/* ---------------- 渲染：顶栏 ---------------- */
function renderTop() {
  const snap = APP.snap;
  const connected = !!(snap && snap.connected);
  const live = (snap && snap.live) || {};
  const stateName = connected ? live.master_state_name || '—' : '未连接';
  const badge = $('masterStateBadge');
  badge.textContent = `总线 ${stateName}`;
  badge.className = `badge ${connected ? stateClass(stateName) : ''}`;
  $('wkcBadge').textContent = connected ? `WKC ${live.wkc}/${live.expected_wkc}` : 'WKC —';
  $('wkcBadge').className = `badge ${connected ? (live.wkc >= live.expected_wkc && live.expected_wkc > 0 ? 'ok' : 'warn') : ''}`;
  const cycle = live.cycle || {};
  $('cycleBadge').textContent = connected
    ? (cycle.enabled ? `${cycle.hz || 0} Hz · ${cycle.period_ms || 0} ms` : '周期已暂停')
    : '— Hz';
  $('cycleBadge').className = `badge ${connected ? (cycle.enabled ? 'info' : 'warn') : ''}`;
  $('cycleBadge').title = connected
    ? `帧间隔 ${Number(cycle.gap_ms || 0).toFixed(1)} ms，最大 ${Number(cycle.gap_max_ms || 0).toFixed(1)} ms；`
      + `>50 ms ${cycle.gap_over50 || 0} 次，已补发 ${cycle.catchup || 0} 帧；`
      + `邮箱轮询 ${cycle.mbx_calls || 0} 次，单次最大 ${Number(cycle.mbx_max_ms || 0).toFixed(1)} ms`
    : '';
  $('btnCycle').textContent = cycle.enabled ? '暂停周期' : '恢复周期';
  $('periodInput').value = cycle.period_ms || 2;
  setBusy(APP.busy);
}

/* ---------------- 渲染：从站列表 ---------------- */
function renderSlaveList() {
  const list = $('slaveList');
  const snap = APP.snap;
  if (!snap || !snap.connected) {
    list.innerHTML = '<div class="empty">尚未连接，请选择网卡后点击“连接”</div>';
    list.dataset.sig = '';
    $('slaveCount').textContent = '0';
    return;
  }
  const slaves = snap.slaves || [];
  $('slaveCount').textContent = String(slaves.length);
  const sig = slaves.map((s) => `${s.pos}:${s.sii.name}`).join('|');
  if (list.dataset.sig !== sig) {
    list.innerHTML = '';
    slaves.forEach((s) => {
      const card = document.createElement('div');
      card.className = 'slave-card';
      card.dataset.pos = s.pos;
      card.innerHTML = `
        <div class="line1">
          <span class="name">${esc(s.sii.name)}</span>
          <span class="pos">#${s.pos}</span>
        </div>
        <div class="line2">
          <span class="chip s-state">—</span>
          <span class="s-al">AL —</span>
          <span class="s-size">—</span>
        </div>`;
      card.addEventListener('click', () => selectSlave(s.pos));
      list.appendChild(card);
    });
    list.dataset.sig = sig;
  }
  slaves.forEach((s) => {
    const card = list.querySelector(`.slave-card[data-pos="${s.pos}"]`);
    if (!card) return;
    card.classList.toggle('selected', s.pos === APP.pos);
    const chip = card.querySelector('.s-state');
    if (!chip) return;
    chip.textContent = s.state_name;
    // 注意：必须保留 s-state 类名，否则下一次渲染 querySelector('.s-state') 会返回 null
    chip.className = `chip s-state ${stateClass(s.state_name)}`;
    const al = card.querySelector('.s-al');
    if (al) {
      al.textContent = `AL ${hex(s.al, 4)}`;
      al.className = `s-al ${s.al ? 'err-text' : ''}`;
    }
    const size = card.querySelector('.s-size');
    if (size) size.textContent = `${s.in_size}B → ${s.out_size}B`;
  });
}

/* ---------------- 渲染：快速诊断 ---------------- */
function renderQuickDiag() {
  const el = $('quickDiag');
  const s = selectedSlave();
  if (!s) { el.textContent = '—'; el.className = 'quick-diag muted'; return; }
  const bad = s.al !== 0;
  const mismatch = s.mismatch;
  el.className = 'quick-diag';
  el.innerHTML = `
    <div>状态：<span class="${bad ? 'err' : 'ok'}">${esc(s.state_name)}</span></div>
    <div>AL：<span class="${bad ? 'err' : 'ok'}">${hex(s.al, 4)} ${esc(s.al_explain.name)}</span></div>
    ${bad ? `<div class="warn">${esc(s.al_explain.detail)}</div>` : ''}
    ${mismatch ? '<div class="warn">SII 与对象字典身份不一致</div>' : '<div class="ok">SII 与对象字典身份一致</div>'}
    <div class="muted">输入 ${s.in_size}B / 输出 ${s.out_size}B</div>`;
}

/* ---------------- 渲染：概览 ---------------- */
function renderOverview() {
  const s = selectedSlave();
  const snap = APP.snap;
  const d = APP.detail;
  const warn = $('ovWarning');

  if (!snap || !snap.connected || !s || !d) {
    $('ovConn').textContent = snap && snap.connected ? `${snap.adapter_desc || snap.adapter}` : '未连接';
    $('ovState').textContent = '—';
    $('ovProc').textContent = '—';
    $('ovIdentity').textContent = '—';
    $('ovSm').textContent = '—';
    $('ovAssign').textContent = '—';
    $('ovSync').textContent = '—';
    warn.classList.add('hidden');
    return;
  }

  const live = snap.live || {};
  $('ovConn').innerHTML = `
    <div><b>${esc(snap.adapter_desc || '—')}</b></div>
    <div class="muted">${esc(snap.adapter || '')}</div>
    <div>IO 映射：<b>${snap.io_size}</b> 字节</div>`;

  $('ovState').innerHTML = `
    <div>状态：<b class="${s.al ? 'err-text' : ''}">${esc(s.state_name)}</b></div>
    <div>AL：<b class="${s.al ? 'err-text' : ''}">${hex(s.al, 4)} ${esc(s.al_explain.name)}</b></div>
    <div class="muted">${esc(s.al_explain.detail)}</div>
    <div>丢失：${s.lost ? '<span class="err-text">是</span>' : '否'}</div>`;

  $('ovProc').innerHTML = `
    <div>WKC：<b>${live.wkc}/${live.expected_wkc}</b></div>
    <div>周期：<b>${live.cycle.hz || 0} Hz / ${live.cycle.period_ms || 0} ms</b></div>
    <div>抖动：<b>${live.cycle.jitter_us || 0} µs</b></div>
    <div>帧数：<b>${live.cycle.frames || 0}</b>（错误 ${live.cycle.errors || 0}）</div>
    <div>帧间隔：<b class="${(live.cycle.gap_max_ms || 0) > 50 ? 'err-text' : ''}">${Number(live.cycle.gap_ms || 0).toFixed(1)} / 最大 ${Number(live.cycle.gap_max_ms || 0).toFixed(1)} ms</b></div>
    <div>停顿：<b class="${live.cycle.gap_over50 ? 'err-text' : ''}">${live.cycle.gap_over50 || 0}</b> 次 &gt;50 ms，补发 <b>${live.cycle.catchup || 0}</b> 帧</div>
    <div>邮箱轮询：<b class="${(live.cycle.mbx_max_ms || 0) > 50 ? 'err-text' : ''}">${live.cycle.mbx_calls || 0}</b> 次，单次最大 ${Number(live.cycle.mbx_max_ms || 0).toFixed(1)} ms（独立线程，0.2 s 一次）</div>`;

  const sii = d.sii || {};
  const ident = d.identity || {};
  const mismatch = !!d.mismatch;
  warn.classList.toggle('hidden', !(mismatch || s.al));
  if (mismatch || s.al) {
    warn.className = 'banner warn';
    warn.innerHTML = [
      mismatch ? '<b>SII/EEPROM 身份与对象字典 0x1018 不一致</b>：主站按 SII 配置 SM/FMMU，但实际对象字典可能不同，易导致 SAFEOP 被拒绝（AL 0x001D/0x001E）。' : '',
      s.al ? `<b>AL 0x${s.al.toString(16).toUpperCase().padStart(4, '0')} ${esc(s.al_explain.name)}</b>：${esc(s.al_explain.detail)}` : '',
    ].filter(Boolean).join('<br>');
  }

  $('ovIdentity').innerHTML = `
    <table>
      <thead><tr><th>来源</th><th>Vendor</th><th>Product</th><th>Revision</th><th>名称</th></tr></thead>
      <tbody>
        <tr><td>SII/EEPROM</td><td class="mono">${hex(sii.man, 8)}</td><td class="mono">${hex(sii.id, 8)}</td><td class="mono">${hex(sii.rev, 8)}</td><td>${esc(sii.name)}</td></tr>
        <tr><td>OD 0x1018</td><td class="mono">${ident.error ? '读取失败' : hex(ident.vendor, 8)}</td><td class="mono">${ident.error ? '—' : hex(ident.product, 8)}</td><td class="mono">${ident.error ? '—' : hex(ident.revision, 8)}</td><td class="muted">${ident.error ? esc(ident.error) : '对象字典自述'}</td></tr>
        <tr><td>ESI 文件</td><td class="mono">${hex(d.esi_device ? d.esi_device.vendor : 0, 8)}</td><td class="mono">${hex(d.esi_device ? d.esi_device.product : 0, 8)}</td><td class="mono">${hex(d.esi_device ? d.esi_device.revision : 0, 8)}</td><td>${esc(d.esi_device ? d.esi_device.name : '')}</td></tr>
      </tbody>
    </table>
    ${mismatch ? '<div class="warn-text small" style="margin-top:8px">⚠ 主站按 SII 识别从站；SII 与 OD/ESI 不一致会直接影响 SM/FMMU 配置。</div>' : ''}`;

  const sm = d.sm || [];
  $('ovSm').innerHTML = `
    <table>
      <thead><tr><th>SM</th><th>Phys</th><th>Len</th><th>Ctrl</th><th>Status</th><th>Act</th></tr></thead>
      <tbody>${sm.map((r) => r.error
        ? `<tr><td>SM${r.index}</td><td colspan="5" class="muted">${esc(r.error)}</td></tr>`
        : `<tr><td>SM${r.index}</td><td class="mono">${hex(r.start, 4)}</td><td class="mono">${r.length}</td><td class="mono">${hex(r.control)}</td><td class="mono">${hex(r.status)}</td><td class="mono">${r.activate}</td></tr>`).join('')}
      </tbody>
    </table>
    <div class="pdo-note">FMMU：${(d.fmmu || []).map((f) => f.error ? `FMMU${f.index} 不可读` : `FMMU${f.index}→${hex(f.phys_start, 4)}/${f.length}B/act${f.activate}`).join(' · ')}</div>`;

  const assign = d.pdo_assign || { rx: [], tx: [] };
  $('ovAssign').innerHTML = `
    <div>RxPDO（输出）分配：${assign.rx.length ? assign.rx.map((v) => `<b class="mono">${hex(v, 4)}</b>`).join(', ') : '<span class="warn-text">未读到</span>'}</div>
    <div>TxPDO（输入）分配：${assign.tx.length ? assign.tx.map((v) => `<b class="mono">${hex(v, 4)}</b>`).join(', ') : '<span class="warn-text">未读到</span>'}</div>
    <div class="pdo-note">映射位宽：输出 ${APP.mapping ? APP.mapping.rx.bits : 0} bit / 输入 ${APP.mapping ? APP.mapping.tx.bits : 0} bit；缓冲：输入 ${s.in_size}B / 输出 ${s.out_size}B</div>
    ${d.mapping_error ? `<div class="err-text small">${esc(d.mapping_error)}</div>` : ''}`;

  const sync = d.sync || {};
  const syncName = (v) => ({ 1: 'Free Run（自由运行）', 2: 'SM 同步', 3: 'DC Sync0', 4: 'DC Sync1' }[v] || `未知 (${v})`);
  $('ovSync').innerHTML = `
    <div>输出同步类型 0x1C32:01 = <b>${sync.output_type != null ? `${sync.output_type} · ${syncName(sync.output_type)}` : '未读到'}</b></div>
    <div>输入同步类型 0x1C33:01 = <b>${sync.input_type != null ? `${sync.input_type} · ${syncName(sync.input_type)}` : '未读到'}</b></div>
    <div>最小周期 0x1C32:05 = <b>${sync.min_cycle != null ? `${sync.min_cycle} µs` : '未读到'}</b></div>
    <div class="pdo-note">DC 未启用时通常为 Free Run；此时仅靠主站周期发送维持看门狗。</div>`;
}

/* ---------------- 渲染：对象字典 ---------------- */
function renderOdList() {
  const el = $('odObjectList');
  const od = APP.od;
  if (!od) {
    el.innerHTML = '<div class="empty">选择从站后加载对象字典</div>';
    $('odCount').textContent = '0';
    return;
  }
  $('odCount').textContent = String(od.count);
  const rows = od.objects.map((o) => `
    <tr data-index="${o.index}" class="${APP.selectedObject === o.index ? 'sel' : ''}">
      <td class="mono">${o.hex}</td>
      <td>${esc(o.name)}</td>
      <td class="mono">${esc(o.esi_name || '')}</td>
      <td class="mono">${o.bits}</td>
      <td class="mono">${esc(o.access || '')}</td>
    </tr>`).join('');
  el.innerHTML = `<table>
    <thead><tr><th>索引</th><th>名称</th><th>ESI 名称</th><th>位长</th><th>访问</th></tr></thead>
    <tbody>${rows}</tbody></table>`;
  el.querySelectorAll('tr[data-index]').forEach((tr) => {
    tr.addEventListener('click', () => loadObject(Number(tr.dataset.index)));
  });
}

async function loadObject(index) {
  if (!APP.pos) return;
  APP.selectedObject = index;
  renderOdList();
  const obj = APP.od.objects.find((o) => o.index === index);
  if (!obj) return;
  $('odDetailTitle').textContent = `${obj.hex}  ${obj.name}  ·  ${obj.esi_name || '—'}`;
  $('btnOdReadAll').disabled = false;
  const subs = buildSubRows(obj);
  renderSubRows(obj, subs, null);
  try {
    const result = await api(`/api/slaves/${APP.pos}/object/${index}`);
    renderSubRows(obj, subs, result.values || {});
  } catch (e) {
    toast(`读取对象 ${obj.hex} 失败：${e.message}`);
  }
}

function buildSubRows(obj) {
  if (obj.sub_info && obj.sub_info.length) {
    return obj.sub_info.map((s) => ({ sub: s.sub, name: s.name, type: s.type, bits: s.bits, access: s.access }));
  }
  if (obj.entries && obj.entries.length) {
    return obj.entries.map((e, i) => ({ sub: i, name: e.name, type: `type=${e.data_type}`, bits: e.bits, access: '' }));
  }
  return [{ sub: 0, name: obj.name, type: obj.esi_name ? '' : `type=${obj.data_type}`, bits: obj.bits, access: obj.access }];
}

function renderSubRows(obj, rows, values) {
  const el = $('odSubs');
  const html = rows.map((r) => {
    const v = values ? values[String(r.sub)] : null;
    let valueText = '—';
    if (v && !v.error) valueText = formatSdoValue(v.data, r.type, r.bits);
    else if (v && v.error) valueText = `<span class="err-text">${esc(v.error)}</span>`;
    const writable = (r.access || '').includes('w') || !r.access;
    return `<tr data-sub="${r.sub}">
      <td class="mono">${hex(r.sub)}</td>
      <td>${esc(r.name || '')}</td>
      <td class="mono">${esc(r.type || '')}</td>
      <td class="mono">${r.bits || ''}</td>
      <td class="mono">${esc(r.access || '')}</td>
      <td class="mono val">${valueText}</td>
      <td>
        <input class="sub-write" placeholder="${writable ? '值 / 0x.. / hex:..' : '只读'}" ${writable ? '' : 'disabled'}>
        <button class="small sub-write-btn" ${writable ? '' : 'disabled'}>写</button>
      </td>
    </tr>`;
  }).join('');
  el.innerHTML = `<table>
    <thead><tr><th>Sub</th><th>名称</th><th>类型</th><th>位长</th><th>访问</th><th>当前值</th><th>写入</th></tr></thead>
    <tbody>${html}</tbody></table>`;
  el.querySelectorAll('tr[data-sub]').forEach((tr) => {
    const sub = Number(tr.dataset.sub);
    const btn = tr.querySelector('.sub-write-btn');
    if (!btn || btn.disabled) return;
    btn.addEventListener('click', async () => {
      const input = tr.querySelector('.sub-write');
      const row = rows.find((r) => r.sub === sub);
      try {
        const bytes = encodeSdoValue(input.value, row.type, row.bits);
        await api('/api/sdo/write', { method: 'POST', body: JSON.stringify({ pos: APP.pos, index: obj.index, sub, data: bytes }) });
        toast(`已写入 ${obj.hex}:${hex(sub)}`, 'ok');
        const result = await api(`/api/slaves/${APP.pos}/object/${obj.index}`);
        renderSubRows(obj, rows, result.values || {});
      } catch (e) {
        toast(`写 SDO 失败：${e.message}`);
      }
    });
  });
}

function formatSdoValue(data, type, bits) {
  if (!data || !data.length) return '(空)';
  const t = (type || '').toUpperCase();
  if (t.includes('STRING')) {
    return esc(new TextDecoder().decode(Uint8Array.from(data)));
  }
  if (t === 'BOOL' || t === 'BOOLEAN') return data[0] ? 'TRUE' : 'FALSE';
  const width = bits && bits <= 64 ? bits : data.length * 8;
  const limit = Math.min(8, Math.ceil(width / 8));
  let value = 0n;
  for (let i = 0; i < limit; i++) value |= BigInt(data[i] || 0) << BigInt(8 * i);
  const signed = /INT/.test(t) && !/UINT/.test(t);
  const signedValue = signed ? BigInt.asIntN(limit * 8, value) : value;
  return `${signedValue} / ${hex(value, limit * 2)}`;
}

function encodeSdoValue(text, type, bits) {
  const t = (type || '').toUpperCase();
  const raw = String(text || '').trim();
  if (!raw) throw new Error('请输入写入值');
  if (t.includes('STRING')) return Array.from(new TextEncoder().encode(raw));
  if (raw.toLowerCase().startsWith('hex:')) return hexToBytes(raw.slice(4));
  let value;
  if (raw.toLowerCase().startsWith('0x')) value = BigInt(raw);
  else if (/^-?\d+$/.test(raw)) value = BigInt(raw);
  else return hexToBytes(raw);
  let bytes = Math.max(1, Math.ceil((bits && bits > 0 ? bits : (value < 0 ? 32 : Math.max(8, value.toString(2).length))) / 8));
  const unsigned = BigInt.asUintN(bytes * 8, value);
  const out = [];
  for (let i = 0; i < bytes; i++) out.push(Number((unsigned >> BigInt(8 * i)) & 0xFFn));
  return out;
}

function hexToBytes(text) {
  const clean = String(text).replace(/0x/gi, '').replace(/[^0-9a-fA-F]/g, '');
  if (!clean.length || clean.length % 2) throw new Error('十六进制数据长度必须为偶数');
  const out = [];
  for (let i = 0; i < clean.length; i += 2) out.push(parseInt(clean.slice(i, i + 2), 16));
  return out;
}

/* ---------------- 演示：IO 控制 ---------------- */
async function sendDemo(byte0) {
  if (!APP.pos) { toast('请先在左侧选择从站'); return; }
  try {
    await api('/api/demo/output', { method: 'POST', body: JSON.stringify({ pos: APP.pos, byte0 }) });
    toast(`已下发 byte0 = 0x${Number(byte0).toString(16).toUpperCase().padStart(2, '0')}`, 'ok');
  } catch (e) {
    toast(e.message);
  }
}

function renderDemo() {
  const s = selectedSlave();
  const demo = (APP.snap && APP.snap.demo) || {};
  if (!s) {
    $('demoOutFrame').textContent = '—';
    $('demoInFrame').textContent = '—';
    $('demoLastCmd').textContent = '—';
    $('demoStateNote').innerHTML = '<span class="warn-text">未连接或未选择从站：请先连接并在左侧选择从站，再切到 OP；否则演示命令不会发送到从站。</span>';
    $('demoKeyLamp').className = 'key-lamp';
    $('demoKeyBit').textContent = '—';
    $('demoKeyPresses').textContent = '—';
    $('demoKeyEdges').textContent = '—';
    $('demoKeyLast').textContent = '—';
    $('demoPushLat').textContent = '—';
    $('demoKeyEvents').innerHTML = '<div class="empty">未连接</div>';
    return;
  }
  $('demoOutFrame').textContent = bytesHex(s.output);
  $('demoInFrame').textContent = bytesHex(s.input);

  const bit = (s.input && s.input.length) ? (s.input[0] & 1) : 0;
  $('demoKeyLamp').className = `key-lamp ${bit ? 'on' : ''}`;
  $('demoKeyBit').textContent = bit ? '1（按下）' : '0（松开）';
  const key = (demo.keys || {})[String(s.pos)] || {};
  $('demoKeyPresses').textContent = key.presses != null ? String(key.presses) : '0';
  $('demoKeyEdges').textContent = key.edges != null ? String(key.edges) : '0';
  $('demoKeyLast').textContent = key.last_change && key.last_change !== '—'
    ? `${key.last_change} ${key.last_edge}` : '—';
  $('demoPushLat').textContent = APP.lastLatency != null ? `${APP.lastLatency.toFixed(1)} ms` : '—';

  const last = demo.last;
  $('demoLastCmd').textContent = last
    ? `0x${Number(last.byte0).toString(16).toUpperCase().padStart(2, '0')} @ ${last.t}` : '—';
  const inOp = (s.state & 0x0F) >= 8;
  $('demoStateNote').innerHTML = inOp
    ? '<span class="ok-text">当前 OP：演示输出随周期帧下发。</span>'
    : `<span class="warn-text">当前 ${esc(s.state_name)}：演示输出不会发送到从站，请先切到 OP。</span>`;

  const events = (demo.key_events || []).filter((e) => e.pos === s.pos).slice().reverse();
  $('demoKeyEvents').innerHTML = events.length
    ? `<table><thead><tr><th>时间</th><th>事件</th><th>bit0</th></tr></thead><tbody>${events.map((e) =>
      `<tr><td class="mono">${esc(e.t)}</td><td>${esc(e.edge)}</td><td class="mono">${e.bit}</td></tr>`).join('')}</tbody></table>`
    : '<div class="empty">暂无按键事件（请在 OP 下按 USER 键）</div>';
}

/* ---------------- 渲染：PDO ---------------- */
function allEntries(mapping, dir) {
  if (!mapping || !mapping[dir]) return [];
  return (mapping[dir].pdos || []).flatMap((p) => p.entries);
}

function buildPdoRows() {
  const mapping = APP.mapping;
  const s = selectedSlave();
  if (!mapping || !s) {
    $('pdoInputs').innerHTML = '<div class="empty">无映射信息</div>';
    $('pdoOutputs').innerHTML = '<div class="empty">无映射信息</div>';
    return;
  }
  const tx = allEntries(mapping, 'tx');
  const rx = allEntries(mapping, 'rx');

  $('pdoInputs').innerHTML = tx.length ? tx.map((e, i) => `
    <div class="signal-row" data-idx="${i}" data-bits="${e.bits}">
      <span class="sig-name">${esc(e.name)}</span>
      <span class="sig-off">bit ${e.bit_offset}</span>
      <span class="sig-off">${e.bits} bit</span>
      <span class="sig-val in-val">—</span>
      <span class="sig-off">${esc(e.index ? hex(e.index, 4) + ':' + hex(e.sub) : '')}</span>
    </div>`).join('') : '<div class="empty">无 TxPDO 映射</div>';

  $('pdoOutputs').innerHTML = rx.length ? rx.map((e, i) => `
    <div class="signal-row" data-idx="${i}" data-bits="${e.bits}" data-off="${e.bit_offset}">
      <span class="sig-name">${esc(e.name)}</span>
      <span class="sig-off">bit ${e.bit_offset}</span>
      <span class="sig-off">${e.bits} bit</span>
      <span class="sig-val out-val">—</span>
      <span class="out-ctrl">${e.bits === 1
        ? `<input type="checkbox" class="out-check" title="勾选/取消立即下发">`
        : `<input type="number" class="out-num" style="width:86px" value="0" title="修改后立即下发">`}</span>
    </div>`).join('') : '<div class="empty">无 RxPDO 映射</div>';

  // 勾选/改动即下发，不再需要单独点「写」；只改该字段，其余位保持当前值
  $('pdoOutputs').querySelectorAll('.signal-row[data-idx]').forEach((row) => {
    const e = rx[Number(row.dataset.idx)];
    const chk = row.querySelector('.out-check');
    const num = row.querySelector('.out-num');
    if (chk) chk.addEventListener('change', () => queuePdoWrite(e, chk.checked ? 1 : 0));
    if (num) num.addEventListener('change', () => queuePdoWrite(e, Number(num.value || 0)));
  });

  $('pdoInSize').textContent = `${s.in_size} B`;
  $('pdoOutSize').textContent = `${s.out_size} B`;
  renderPdoMapInfo();
}

/* ---------------- RxPDO 输出写入：最新意图优先 ---------------- */
const PDO_WRITE = { busy: false, pending: new Map() };

function queuePdoWrite(entry, value) {
  // 同一字段只保留最新一次意图；连续快速勾选会被合并，最后一次一定生效
  PDO_WRITE.pending.set(`${entry.bit_offset}:${entry.bits}`, { entry, value });
  drainPdoWrites();
}

async function drainPdoWrites() {
  if (PDO_WRITE.busy) return;
  PDO_WRITE.busy = true;
  try {
    while (PDO_WRITE.pending.size) {
      const [key, item] = PDO_WRITE.pending.entries().next().value;
      PDO_WRITE.pending.delete(key);
      await api('/api/pdo/write', {
        method: 'POST',
        body: JSON.stringify({
          pos: APP.pos,
          bit_offset: item.entry.bit_offset,
          bit_length: item.entry.bits,
          value: item.value,
        }),
      });
    }
  } catch (e) {
    toast(e.message);
    PDO_WRITE.pending.clear();
    // 触发一次兜底快照，让界面回到从站真实输出值
    APP.lastDataAt = 0;
  } finally {
    PDO_WRITE.busy = false;
  }
}

function renderPdoLive() {
  const s = selectedSlave();
  const warn = $('pdoWarning');
  if (!s || !APP.snap) return;
  const mapping = APP.mapping;
  $('pdoInSize').textContent = `${s.in_size} B`;
  $('pdoOutSize').textContent = `${s.out_size} B`;
  $('pdoRawIn').textContent = bytesHex(s.input);
  $('pdoRawOut').textContent = bytesHex(s.output);

  const invalid = (s.state & 0x0F) < 4;
  warn.classList.toggle('hidden', !invalid);
  if (invalid) {
    warn.className = 'banner warn';
    warn.innerHTML = `当前状态 <b>${esc(s.state_name)}</b>，过程数据未生效（需 SAFEOP 或 OP）。` +
      (s.al ? ` AL 0x${s.al.toString(16).toUpperCase().padStart(4, '0')} ${esc(s.al_explain.name)}：${esc(s.al_explain.detail)}` : '');
  }

  const inRows = $('pdoInputs').querySelectorAll('.signal-row[data-idx]');
  (s.inputs || []).forEach((sig, i) => {
    const row = inRows[i];
    if (!row) return;
    const cell = row.querySelector('.in-val');
    if (sig.bits === 1) cell.innerHTML = `<span class="led ${sig.value ? 'on' : ''}"></span>${sig.value ? '1' : '0'}`;
    else cell.textContent = String(sig.value);
  });

  const outRows = $('pdoOutputs').querySelectorAll('.signal-row[data-idx]');
  (s.outputs || []).forEach((sig, i) => {
    const row = outRows[i];
    if (!row) return;
    row.querySelector('.out-val').textContent = String(sig.value);
    if (sig.bits === 1) {
      const chk = row.querySelector('.out-check');
      if (chk && document.activeElement !== chk) chk.checked = !!sig.value;
    } else {
      const num = row.querySelector('.out-num');
      if (num && document.activeElement !== num) num.value = String(sig.value);
    }
  });
}

function renderPdoMapInfo() {
  const mapping = APP.mapping;
  const el = $('pdoMapInfo');
  if (!mapping) { el.textContent = '—'; return; }
  const part = (label, dir) => {
    const pdos = mapping[dir].pdos || [];
    return `<div><b>${label}</b>（${mapping[dir].bits} bit）：` + (pdos.length ? pdos.map((p) => `${hex(p.pdo, 4)} [${p.entries.map((e) => `${esc(e.name)}:${e.bits}`).join(', ')}]`).join('；') : '无') + '</div>';
  };
  el.innerHTML = part('RxPDO / 输出', 'rx') + part('TxPDO / 输入', 'tx');
}

/* ---------------- 渲染：诊断 ---------------- */
function renderDiag() {
  const snap = APP.snap;
  const s = selectedSlave();
  const detail = APP.detail;
  $('diagSummary').innerHTML = (snap && snap.connected && s)
    ? `<div>从站 #${s.pos} <b>${esc(s.sii.name)}</b> · 状态 <b class="${s.al ? 'err-text' : 'ok-text'}">${esc(s.state_name)}</b></div>
       <div>AL 0x${s.al.toString(16).toUpperCase().padStart(4, '0')} <b>${esc(s.al_explain.name)}</b></div>
       <div class="muted">${esc(s.al_explain.detail)}</div>
       ${s.mismatch ? '<div class="warn-text">SII 与对象字典身份不一致，主站按 SII 配置 SM/FMMU。</div>' : ''}
       ${detail && detail.mapping_error ? `<div class="err-text">映射读取：${esc(detail.mapping_error)}</div>` : ''}`
    : '<span class="muted">未连接</span>';

  const al = (snap && snap.al_history) || [];
  $('alHistory').innerHTML = al.length ? `<table>
    <thead><tr><th>时间</th><th>从站</th><th>状态</th><th>AL 码</th><th>解释</th></tr></thead>
    <tbody>${al.slice().reverse().map((e) => `<tr><td class="mono">${esc(e.t)}</td><td>#${e.pos}</td><td>${esc(e.state_name)}</td><td class="mono ${e.al ? 'err-text' : ''}">${hex(e.al, 4)}</td><td>${esc(e.al_text)}</td></tr>`).join('')}</tbody></table>`
    : '<div class="empty">暂无状态码变化</div>';

  const emcy = (snap && snap.emcy) || [];
  $('emcyList').innerHTML = emcy.length ? `<table>
    <thead><tr><th>时间</th><th>从站</th><th>错误码</th><th>寄存器</th><th>数据</th><th>解释</th></tr></thead>
    <tbody>${emcy.slice().reverse().map((e) => `<tr><td class="mono">${esc(e.t)}</td><td>#${e.pos}</td><td class="mono">${hex(e.code, 4)}</td><td class="mono">${hex(e.reg, 2)}</td><td class="mono">${hex(e.b1, 2)} / ${hex(e.w1, 4)} / ${hex(e.w2, 4)}</td><td>${esc(e.text)}</td></tr>`).join('')}</tbody></table>`
    : '<div class="empty">暂无 EMCY 消息</div>';

  renderLogs();
}

function renderLogs() {
  const el = $('logView');
  if (!APP.snap) { el.innerHTML = '<div class="empty">—</div>'; return; }
  const logs = APP.snap.logs || [];
  const nearBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 30;
  el.innerHTML = logs.map((l) => `<div class="log-line ${l.level}"><span class="t">[${esc(l.t)}]</span> <span class="m">${esc(l.msg)}</span></div>`).join('') || '<div class="empty">暂无日志</div>';
  if (nearBottom) el.scrollTop = el.scrollHeight;
}

/* ---------------- 快照分发 ---------------- */
// 每个面板独立渲染：任一面板抛异常只影响自己，不会中断后续面板（例如 IO 演示页）。
// 不再按当前标签门控——隐藏的面板也保持同步，切过去即可看到最新值。
const PANEL_RENDERERS = [
  ['顶栏', () => renderTop()],
  ['从站列表', () => renderSlaveList()],
  ['快速诊断', () => renderQuickDiag()],
  ['概览', () => renderOverview()],
  ['IO 演示', () => renderDemo()],
  ['PDO 监视', () => renderPdoLive()],
  ['诊断 / EMCY', () => renderDiag()],
];

function renderSnapshot() {
  const t0 = performance.now();
  PANEL_RENDERERS.forEach(([name, fn]) => {
    try {
      fn();
    } catch (e) {
      APP.renderError = `${name}: ${e && e.message ? e.message : e}`;
      console.error(`[render] 面板「${name}」渲染失败:`, e);
    }
  });
  APP.renderMs = performance.now() - t0;
}

/* ---------------- 事件绑定 ---------------- */
function bind() {
  $('btnRefreshAdapters').addEventListener('click', loadAdapters);
  $('btnConnect').addEventListener('click', connect);
  $('btnDisconnect').addEventListener('click', disconnect);
  document.querySelectorAll('.state-btn').forEach((btn) => btn.addEventListener('click', () => requestState(btn.dataset.state)));
  $('btnAck').addEventListener('click', () => ackState());
  document.querySelectorAll('.demo-cmd').forEach((btn) => btn.addEventListener('click', () => sendDemo(Number(btn.dataset.byte))));
  $('btnCycle').addEventListener('click', async () => {
    const enabled = !(APP.snap && APP.snap.live && APP.snap.live.cycle.enabled);
    try {
      await api('/api/cycle', { method: 'POST', body: JSON.stringify({ enabled }) });
    } catch (e) { toast(e.message); }
  });
  $('periodInput').addEventListener('change', async () => {
    const value = Number($('periodInput').value);
    try { await api('/api/cycle', { method: 'POST', body: JSON.stringify({ period_ms: value }) }); }
    catch (e) { toast(e.message); }
  });

  document.querySelectorAll('.tabs button').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.tabs button').forEach((b) => b.classList.remove('active'));
      document.querySelectorAll('.tab').forEach((t) => t.classList.remove('active'));
      btn.classList.add('active');
      APP.tab = btn.dataset.tab;
      $(`tab-${APP.tab}`).classList.add('active');
      if (APP.tab === 'overview') renderOverview();
      if (APP.tab === 'demo') { renderDemo(); }
      if (APP.tab === 'pdo') { buildPdoRows(); renderPdoLive(); }
      if (APP.tab === 'diag') renderDiag();
    });
  });

  $('btnOdReload').addEventListener('click', async () => {
    if (!APP.pos) { toast('请先选择从站'); return; }
    try {
      APP.od = await api(`/api/slaves/${APP.pos}/od`);
      renderOdList();
      toast('对象字典已刷新', 'ok');
    } catch (e) { toast(e.message); }
  });
  $('btnOdReadAll').addEventListener('click', () => { if (APP.selectedObject != null) loadObject(APP.selectedObject); });

  $('btnRawRead').addEventListener('click', async () => {
    try {
      const index = parseIndex($('rawIndex').value);
      const sub = parseInt($('rawSub').value || '0', 10);
      const data = await api(`/api/sdo?pos=${APP.pos}&index=${index}&sub=${sub}`);
      $('rawResult').textContent = `${hex(index, 4)}:${hex(sub)} = ${bytesHex(data.data)} (${data.length} B)`;
    } catch (e) { $('rawResult').textContent = e.message; }
  });
  $('btnRawWrite').addEventListener('click', async () => {
    try {
      const index = parseIndex($('rawIndex').value);
      const sub = parseInt($('rawSub').value || '0', 10);
      const data = hexToBytes($('rawData').value);
      await api('/api/sdo/write', { method: 'POST', body: JSON.stringify({ pos: APP.pos, index, sub, data }) });
      $('rawResult').textContent = `写入成功 ${hex(index, 4)}:${hex(sub)} ← ${bytesHex(data)}`;
    } catch (e) { $('rawResult').textContent = `写入失败：${e.message}`; }
  });

  $('btnPdoRawWrite').addEventListener('click', async () => {
    try {
      const data = hexToBytes($('pdoRawInput').value);
      await api('/api/pdo/raw', { method: 'POST', body: JSON.stringify({ pos: APP.pos, data }) });
      toast('整帧输出已写入', 'ok');
    } catch (e) { toast(e.message); }
  });

  $('btnClearEmcy').addEventListener('click', async () => {
    try { await api('/api/emcy/clear', { method: 'POST', body: '{}' }); toast('EMCY 已清空', 'ok'); }
    catch (e) { toast(e.message); }
  });
}

/* ---------------- 推送应用（事件驱动） ---------------- */
const STATE_LABELS = { 0: 'NONE', 1: 'INIT', 2: 'PREOP', 4: 'SAFEOP', 8: 'OP' };

function stateNameOf(raw) {
  const base = STATE_LABELS[raw & 0x0F] || `0x${(raw & 0x0F).toString(16).toUpperCase()}`;
  return base + ((raw & 0x10) ? '+ERR' : '');
}

function applyPdoDelta(d) {
  if (!APP.snap) return;
  const s = (APP.snap.slaves || []).find((x) => x.pos === d.pos);
  if (!s) return;
  if (d.input) s.input = d.input;
  if (d.output) s.output = d.output;
  if (d.state != null) { s.state = d.state; s.state_name = stateNameOf(d.state); }
  if (d.al != null) { s.al = d.al; }
  renderSnapshot();
}

function applyKeyEvent(ev) {
  if (!APP.snap) return;
  if (!APP.snap.demo) APP.snap.demo = { last: null, keys: {}, key_events: [] };
  if (!APP.snap.demo.keys) APP.snap.demo.keys = {};
  if (!APP.snap.demo.key_events) APP.snap.demo.key_events = [];
  const key = APP.snap.demo.keys[String(ev.pos)] || {};
  key.bit = ev.bit;
  key.presses = ev.presses;
  key.edges = ev.edges;
  key.last_change = ev.t;
  key.last_edge = ev.edge;
  APP.snap.demo.keys[String(ev.pos)] = key;
  APP.snap.demo.key_events.push({ t: ev.t, pos: ev.pos, bit: ev.bit, edge: ev.edge });
  if (APP.snap.demo.key_events.length > 60) APP.snap.demo.key_events = APP.snap.demo.key_events.slice(-60);
  const s = (APP.snap.slaves || []).find((x) => x.pos === ev.pos);
  if (s && ev.input && ev.input.length) s.input = ev.input;
  renderSnapshot();
}

function noteLatency(payload) {
  if (!payload || payload.ts_ms == null) return;
  APP.lastLatency = Math.max(0, Date.now() - payload.ts_ms);
  APP.lastLatencyAt = Date.now();
}

function setPushBadge(state) {
  const badge = $('pushBadge');
  if (!badge) return;
  if (state === 'live') {
    APP.pushLive = true;
    const lat = APP.lastLatency != null ? ` · ${APP.lastLatency.toFixed(1)} ms` : '';
    badge.textContent = `推送 已连接${lat}`;
    badge.className = 'badge ok';
  } else if (state === 'fallback') {
    APP.pushLive = false;
    badge.textContent = '推送 中断·兜底轮询';
    badge.className = 'badge warn';
  } else {
    APP.pushLive = false;
    badge.textContent = '推送 连接中…';
    badge.className = 'badge';
  }
}

function startEvents() {
  const es = new EventSource('/api/events');
  APP.lastDataAt = Date.now();
  es.onopen = () => { setPushBadge('live'); APP.lastDataAt = Date.now(); };
  es.addEventListener('snapshot', (event) => {
    try {
      APP.snap = JSON.parse(event.data);
      APP.lastDataAt = Date.now();
      noteLatency(APP.snap);
      renderSnapshot();
      setPushBadge('live');
      console.log(`[push] snapshot 服务端→页面 ${(APP.lastLatency || 0).toFixed(1)} ms，渲染 ${(APP.renderMs || 0).toFixed(1)} ms`);
    } catch (e) { console.error('[push] snapshot 处理失败:', e); }
  });
  es.addEventListener('key', (event) => {
    try {
      const payload = JSON.parse(event.data);
      noteLatency(payload);
      applyKeyEvent(payload);
      APP.lastDataAt = Date.now();
      if (APP.pushLive) setPushBadge('live');
      console.log(`[push] key 服务端→页面 ${(APP.lastLatency || 0).toFixed(1)} ms，渲染 ${(APP.renderMs || 0).toFixed(1)} ms`);
    } catch (e) { console.error('[push] key 事件处理失败:', e); }
  });
  es.addEventListener('pdo', (event) => {
    try {
      const payload = JSON.parse(event.data);
      noteLatency(payload);
      applyPdoDelta(payload);
      APP.lastDataAt = Date.now();
      if (APP.pushLive) setPushBadge('live');
    } catch (e) { console.error('[push] pdo 事件处理失败:', e); }
  });
  es.onerror = () => {
    // EventSource 会按 retry 自动重连；这里只更新连接状态
    setPushBadge('fallback');
  };

  // 低频兜底：推送静默超过 1 s 就 REST 拉一次快照（防丢包/断线）
  setInterval(async () => {
    if (Date.now() - (APP.lastDataAt || 0) <= 1000) return;
    if (APP.fetchingSnap) return;
    APP.fetchingSnap = true;
    setPushBadge('fallback');
    try {
      const data = await api('/api/snapshot');
      APP.snap = data;
      APP.lastDataAt = Date.now();
      renderSnapshot();
    } catch (e) { /* 后端不可用，保持兜底状态 */ }
    finally { APP.fetchingSnap = false; }
  }, 1000);
}

/* ---------------- 启动 ---------------- */
(async function init() {
  bind();
  await loadAdapters();
  startEvents();
})();
