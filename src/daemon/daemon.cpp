#include "daemon/daemon.hpp"
#include "audio/audio_engine.hpp"
#include "common/i18n.hpp"
#include "platform/platform.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline void closesocket(SOCKET s) { close(s); }
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace aoh {

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            int hi = std::isdigit(static_cast<unsigned char>(s[i + 1]))
                         ? (s[i + 1] - '0')
                         : (std::tolower(static_cast<unsigned char>(s[i + 1])) - 'a' + 10);
            int lo = std::isdigit(static_cast<unsigned char>(s[i + 2]))
                         ? (s[i + 2] - '0')
                         : (std::tolower(static_cast<unsigned char>(s[i + 2])) - 'a' + 10);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string& body) {
    std::unordered_map<std::string, std::string> m;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        std::string pair = body.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            m[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return m;
}

SOCKET create_loopback_listener(int requested_port, int& bound_port) {
    auto try_bind = [&](int port_to_bind) -> SOCKET {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;

        int opt = 1;
#ifdef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port_to_bind));

        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            bound_port = ntohs(addr.sin_port);
        } else {
            bound_port = port_to_bind;
        }
        return s;
    };

    SOCKET s = try_bind(requested_port);
    if (s == INVALID_SOCKET && requested_port != 0) {
        s = try_bind(0); // Automatic free port fallback if requested port is busy
    }
    return s;
}

bool send_all(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

const char* kWebUiHtml = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>A-OH 终端音效设置</title>
<style>
  :root { --bg:#0f172a; --card:#1e293b; --border:#334155; --text:#f1f5f9; --muted:#94a3b8; --accent:#38bdf8; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Microsoft YaHei",sans-serif; background:var(--bg); color:var(--text); padding:24px; }
  .wrap { max-width:820px; margin:0 auto; }
  h1 { margin:0 0 6px; font-size:24px; }
  .sub { color:var(--muted); font-size:13px; margin-bottom:18px; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:12px; padding:18px; margin-bottom:16px; }
  .card h2 { margin:0 0 14px; font-size:16px; color:var(--accent); }
  .row { display:grid; grid-template-columns:150px 1fr auto; gap:10px; align-items:center; margin-bottom:12px; }
  .row-2 { display:grid; grid-template-columns:150px 1fr; gap:10px; align-items:center; margin-bottom:12px; }
  label { font-size:13px; color:#cbd5e1; }
  select, input[type=text], input[type=number] {
    width:100%; background:#0f172a; border:1px solid var(--border); color:var(--text);
    padding:8px 10px; border-radius:8px; font-size:13px;
  }
  input[type=range] { width:100%; }
  button {
    background:#334155; color:var(--text); border:1px solid #475569; padding:8px 13px;
    border-radius:8px; cursor:pointer; font-size:13px; font-weight:500;
  }
  button:hover { background:#475569; }
  button.primary { background:#0284c7; border-color:#0ea5e9; }
  button.primary:hover { background:#0369a1; }
  button.ok { background:#15803d; border-color:#22c55e; }
  .actions { display:flex; flex-wrap:wrap; gap:10px; align-items:center; }
  .pill { display:inline-block; font-size:12px; padding:3px 10px; border-radius:999px; background:#0f172a; border:1px solid var(--border); color:var(--muted); }
  .status-bar { display:flex; flex-wrap:wrap; gap:8px; margin-bottom:16px; }
  .toast { position:fixed; right:20px; bottom:20px; background:#0f172a; border:1px solid var(--accent); color:var(--text); padding:10px 16px; border-radius:8px; font-size:13px; display:none; }
  @media (max-width:640px) { .row, .row-2 { grid-template-columns:1fr; } }
</style>
</head>
<body>
<div class="wrap">
  <div style="display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap;">
    <div>
      <h1 id="tTitle">A-OH 终端 / 批处理音效助手</h1>
      <div class="sub" id="tSub">系统原生 MP3 播放 · 选中后常驻内存不释放 · 多次触发打断不重叠</div>
    </div>
    <div style="min-width:150px;">
      <select id="language" onchange="onLangChange()" aria-label="Language">
        <option value="auto" id="optLangAuto">语言: 自动 (Auto)</option>
        <option value="zh">简体中文</option>
        <option value="en">English</option>
      </select>
    </div>
  </div>

  <div class="status-bar">
    <span class="pill" id="stBackend">...</span>
    <span class="pill" id="stRam">...</span>
    <span class="pill" id="stErr" style="display:none"></span>
  </div>

  <div class="card">
    <h2 id="tSecExit">1. 批处理 / 终端退出音效 (Exit)</h2>
    <div class="row-2">
      <label id="tExitModeLbl">播放模式</label>
      <select id="exit_mode">
        <option value="random" id="optExitRandom">随机从下方音效槽抽取 (Random)</option>
        <option value="ding" id="optExitDing">内置清脆滴滴声 (零依赖)</option>
        <option value="mp3" id="optExitMp3">指定单个 MP3 / WAV (常驻内存)</option>
      </select>
    </div>
    <div class="row">
      <label id="tExitSingleLbl">指定单个 MP3</label>
      <input type="text" id="exit_mp3_path" placeholder="仅在[指定单个 MP3]模式下使用" />
      <button type="button" id="btnBrowseExit" onclick="pickSingleFile('exit_mp3_path')">浏览...</button>
    </div>
    <div class="actions">
      <button class="ok" type="button" id="btnTestExit" onclick="testPlay('exit')">试听退出音效 (连续点可测试打断/随机)</button>
    </div>
  </div>

  <div class="card">
    <h2 id="tSecIdle">2. 终端闲置音效 (Idle)</h2>
    <div class="row-2">
      <label id="tIdleModeLbl">播放模式</label>
      <select id="idle_mode">
        <option value="random" id="optIdleRandom">随机从下方音效槽抽取 (Random)</option>
        <option value="ding" id="optIdleDing">内置清脆滴滴声 (零依赖)</option>
        <option value="mp3" id="optIdleMp3">指定单个 MP3 / WAV (常驻内存)</option>
      </select>
    </div>
    <div class="row">
      <label id="tIdleSingleLbl">指定单个 MP3</label>
      <input type="text" id="idle_mp3_path" placeholder="仅在[指定单个 MP3]模式下使用" />
      <button type="button" id="btnBrowseIdle" onclick="pickSingleFile('idle_mp3_path')">浏览...</button>
    </div>
    <div class="actions">
      <button class="ok" type="button" id="btnTestIdle" onclick="testPlay('idle')">试听闲置音效 (连续点可测试打断/随机)</button>
      <button type="button" id="btnStop" onclick="stopPlay()">立即静音打断</button>
    </div>
  </div>

  <div class="card">
    <h2 id="tSecPool">3. 随机音效槽位池 (默认 5 个，可增减 · 选中即常驻内存)</h2>
    <div id="slotList"></div>
    <div class="actions" style="margin-top:10px;">
      <button type="button" id="btnAddSlot" onclick="addSlot()">+ 添加槽位</button>
      <button type="button" id="btnDelLast" onclick="removeLastSlot()">- 减少最后槽位</button>
      <button type="button" id="btnResetSlots" onclick="resetBuiltinSlots()">恢复内置 1~5 号</button>
      <button class="primary" type="button" id="btnSaveSlots" onclick="saveConfig()">保存槽位并加载到内存</button>
    </div>
  </div>

  <div class="card">
    <h2 id="tSecSys">4. 监听与系统设置</h2>
    <div class="row">
      <label><span id="tVolLbl">全局音量</span> (<span id="volText">85</span>%)</label>
      <input type="range" id="volume" min="0" max="100" value="85" oninput="document.getElementById('volText').textContent=this.value" />
      <span></span>
    </div>
    <div class="row-2">
      <label id="tWatchLbl">全局监听终端退出</label>
      <div><input type="checkbox" id="watch_enabled" /> <span id="tWatchHint" style="font-size:13px;color:var(--muted)">后台自动监控下列终端/批处理进程退出</span></div>
    </div>
    <div class="row-2">
      <label id="tProcsLbl">监听进程名 (逗号分隔)</label>
      <input type="text" id="watch_processes" placeholder="cmd.exe,powershell.exe,pwsh.exe" />
    </div>
    <div class="row-2">
      <label id="tMinMsLbl">最短存活过滤 (毫秒)</label>
      <input type="number" id="min_process_ms" min="0" max="600000" />
    </div>
    <div class="row-2">
      <label id="tIdleSecLbl">默认闲置阈值 (秒)</label>
      <input type="number" id="idle_seconds" min="1" max="86400" />
    </div>
    <div class="row-2">
      <label id="tAutoLbl">开机自启动</label>
      <div><input type="checkbox" id="autostart" /> <span id="tAutoHint" style="font-size:13px;color:var(--muted)">登录系统时自动后台启动 A-OH</span></div>
    </div>
    <div class="actions" style="margin-top:14px;">
      <button class="primary" type="button" id="btnSaveAll" onclick="saveConfig()">保存配置并常驻内存</button>
      <button type="button" id="btnRefresh" onclick="loadState()">刷新状态</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
const I18N = {
  zh: {
    pageTitle: 'A-OH 终端音效设置',
    title: 'A-OH 终端 / 批处理音效助手',
    sub: '系统原生 MP3 播放 · 选中后常驻内存不释放 · 多次触发打断不重叠',
    langAuto: '语言: 自动 (Auto)',
    secExit: '1. 批处理 / 终端退出音效 (Exit)',
    secIdle: '2. 终端闲置音效 (Idle)',
    secPool: '3. 随机音效槽位池 (默认 5 个，可增减 · 选中即常驻内存)',
    secSys: '4. 监听与系统设置',
    modeLbl: '播放模式',
    optRandom: '随机从下方音效槽抽取 (Random)',
    optDing: '内置清脆滴滴声 (零依赖)',
    optMp3: '指定单个 MP3 / WAV (常驻内存)',
    singleLbl: '指定单个 MP3',
    singlePh: '仅在[指定单个 MP3]模式下使用',
    browse: '浏览...',
    testExit: '试听退出音效 (连续点可测试打断/随机)',
    testIdle: '试听闲置音效 (连续点可测试打断/随机)',
    stop: '立即静音打断',
    addSlot: '+ 添加槽位',
    delLast: '- 减少最后槽位',
    resetSlots: '恢复内置 1~5 号',
    saveSlots: '保存槽位并加载到内存',
    volLbl: '全局音量',
    watchLbl: '全局监听终端退出',
    watchHint: '后台自动监控下列终端/批处理进程退出',
    procsLbl: '监听进程名 (逗号分隔)',
    minMsLbl: '最短存活过滤 (毫秒)',
    idleSecLbl: '默认闲置阈值 (秒)',
    autoLbl: '开机自启动',
    autoHint: '登录系统时自动后台启动 A-OH',
    saveAll: '保存配置并常驻内存',
    refresh: '刷新状态',
    slotLabel: (n) => `槽位 #${n}`,
    slotPh: (n) => `选择第 ${n} 个随机 MP3/WAV 文件`,
    playSlot: '试听',
    delSlot: '删除',
    backendPrefix: '音频后端: ',
    ramText: (cnt, kb) => `内存常驻缓存: ${cnt} 个 (${kb} KB)`,
    errPrefix: '提示: ',
    savedOk: '已保存，所有选中 MP3 已常驻内存',
    saveFail: '保存失败: ',
    pickPrompt: '请在系统窗口中选择 MP3 / 音频文件...',
    pickSlotPrompt: (n) => `请选择槽位 #${n} 的音频文件...`
  },
  en: {
    pageTitle: 'A-OH Sound Settings',
    title: 'A-OH Terminal & Batch Sound Notifier',
    sub: 'Native MP3 playback · Resident in RAM · Interruptible non-overlapping audio',
    langAuto: 'Language: Auto',
    secExit: '1. Terminal / Batch Exit Sound (Exit)',
    secIdle: '2. Terminal Idle Sound (Idle)',
    secPool: '3. Random Sound Slots (5 by default, add/remove freely · kept in RAM)',
    secSys: '4. Monitoring & System Settings',
    modeLbl: 'Playback Mode',
    optRandom: 'Random from Sound Slots below',
    optDing: 'Built-in Crisp Ding (Zero external files)',
    optMp3: 'Single MP3 / WAV File (Resident in RAM)',
    singleLbl: 'Single MP3 Path',
    singlePh: 'Used only in Single MP3 mode',
    browse: 'Browse...',
    testExit: 'Test Exit Sound (Click rapidly to test interrupt/random)',
    testIdle: 'Test Idle Sound (Click rapidly to test interrupt/random)',
    stop: 'Stop Audio',
    addSlot: '+ Add Slot',
    delLast: '- Remove Last',
    resetSlots: 'Reset Built-in 1~5',
    saveSlots: 'Save Slots & Load to RAM',
    volLbl: 'Master Volume',
    watchLbl: 'Watch Terminal Exit',
    watchHint: 'Monitor listed terminal/batch processes in background',
    procsLbl: 'Process Names (CSV)',
    minMsLbl: 'Min Lifetime Filter (ms)',
    idleSecLbl: 'Default Idle Timeout (s)',
    autoLbl: 'Launch at Startup',
    autoHint: 'Automatically start A-OH in background on login',
    saveAll: 'Save Settings & Cache in RAM',
    refresh: 'Refresh',
    slotLabel: (n) => `Slot #${n}`,
    slotPh: (n) => `Choose MP3/WAV for random slot #${n}`,
    playSlot: 'Play',
    delSlot: 'Remove',
    backendPrefix: 'Audio Backend: ',
    ramText: (cnt, kb) => `RAM Cache: ${cnt} file(s) (${kb} KB)`,
    errPrefix: 'Note: ',
    savedOk: 'Saved! Selected MP3 files are cached in RAM',
    saveFail: 'Save failed: ',
    pickPrompt: 'Please choose an MP3/audio file in the system dialog...',
    pickSlotPrompt: (n) => `Choose audio file for Slot #${n}...`
  }
};

let currentSlots = ['', '', '', '', ''];
let lastState = null;

function activeLangKey() {
  const sel = document.getElementById('language').value;
  if (sel === 'zh' || sel === 'en') return sel;
  if (lastState && (lastState.resolved_lang === 'zh' || lastState.resolved_lang === 'en')) {
    return lastState.resolved_lang;
  }
  return (navigator.language || '').toLowerCase().startsWith('zh') ? 'zh' : 'en';
}

function tr() { return I18N[activeLangKey()]; }

function applyI18n() {
  const t = tr();
  document.title = t.pageTitle;
  document.documentElement.lang = activeLangKey() === 'zh' ? 'zh-CN' : 'en';
  document.getElementById('tTitle').textContent = t.title;
  document.getElementById('tSub').textContent = t.sub;
  document.getElementById('optLangAuto').textContent = t.langAuto;
  document.getElementById('tSecExit').textContent = t.secExit;
  document.getElementById('tSecIdle').textContent = t.secIdle;
  document.getElementById('tSecPool').textContent = t.secPool;
  document.getElementById('tSecSys').textContent = t.secSys;
  document.getElementById('tExitModeLbl').textContent = t.modeLbl;
  document.getElementById('tIdleModeLbl').textContent = t.modeLbl;
  document.getElementById('optExitRandom').textContent = t.optRandom;
  document.getElementById('optIdleRandom').textContent = t.optRandom;
  document.getElementById('optExitDing').textContent = t.optDing;
  document.getElementById('optIdleDing').textContent = t.optDing;
  document.getElementById('optExitMp3').textContent = t.optMp3;
  document.getElementById('optIdleMp3').textContent = t.optMp3;
  document.getElementById('tExitSingleLbl').textContent = t.singleLbl;
  document.getElementById('tIdleSingleLbl').textContent = t.singleLbl;
  document.getElementById('exit_mp3_path').placeholder = t.singlePh;
  document.getElementById('idle_mp3_path').placeholder = t.singlePh;
  document.getElementById('btnBrowseExit').textContent = t.browse;
  document.getElementById('btnBrowseIdle').textContent = t.browse;
  document.getElementById('btnTestExit').textContent = t.testExit;
  document.getElementById('btnTestIdle').textContent = t.testIdle;
  document.getElementById('btnStop').textContent = t.stop;
  document.getElementById('btnAddSlot').textContent = t.addSlot;
  document.getElementById('btnDelLast').textContent = t.delLast;
  document.getElementById('btnResetSlots').textContent = t.resetSlots;
  document.getElementById('btnSaveSlots').textContent = t.saveSlots;
  document.getElementById('tVolLbl').textContent = t.volLbl;
  document.getElementById('tWatchLbl').textContent = t.watchLbl;
  document.getElementById('tWatchHint').textContent = t.watchHint;
  document.getElementById('tProcsLbl').textContent = t.procsLbl;
  document.getElementById('tMinMsLbl').textContent = t.minMsLbl;
  document.getElementById('tIdleSecLbl').textContent = t.idleSecLbl;
  document.getElementById('tAutoLbl').textContent = t.autoLbl;
  document.getElementById('tAutoHint').textContent = t.autoHint;
  document.getElementById('btnSaveAll').textContent = t.saveAll;
  document.getElementById('btnRefresh').textContent = t.refresh;
  if (lastState) {
    document.getElementById('stBackend').textContent = t.backendPrefix + lastState.backend;
    const kb = Math.round((lastState.cached_bytes || 0) / 1024);
    document.getElementById('stRam').textContent = t.ramText(lastState.cached_count, kb);
    const errEl = document.getElementById('stErr');
    errEl.textContent = lastState.last_error ? (t.errPrefix + lastState.last_error) : '';
    errEl.style.display = lastState.last_error ? 'inline-block' : 'none';
  }
  renderSlots();
}

function showToast(msg) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.display = 'block';
  clearTimeout(window._tt);
  window._tt = setTimeout(() => el.style.display = 'none', 2600);
}

function renderSlots() {
  const t = tr();
  const box = document.getElementById('slotList');
  box.innerHTML = '';
  currentSlots.forEach((val, idx) => {
    const row = document.createElement('div');
    row.className = 'row';
    row.style.gridTemplateColumns = '90px 1fr auto auto auto';

    const lbl = document.createElement('label');
    lbl.textContent = t.slotLabel(idx + 1);

    const inp = document.createElement('input');
    inp.type = 'text';
    inp.value = val || '';
    inp.placeholder = t.slotPh(idx + 1);
    inp.oninput = () => { currentSlots[idx] = inp.value; };

    const btnPick = document.createElement('button');
    btnPick.type = 'button';
    btnPick.textContent = t.browse;
    btnPick.onclick = () => pickSlotFile(idx);

    const btnPlay = document.createElement('button');
    btnPlay.type = 'button';
    btnPlay.className = 'ok';
    btnPlay.textContent = t.playSlot;
    btnPlay.onclick = () => testSlot(idx);

    const btnDel = document.createElement('button');
    btnDel.type = 'button';
    btnDel.textContent = t.delSlot;
    btnDel.onclick = () => removeSlot(idx);

    row.append(lbl, inp, btnPick, btnPlay, btnDel);
    box.appendChild(row);
  });
}

function addSlot() {
  currentSlots.push('');
  renderSlots();
}

function removeSlot(idx) {
  if (currentSlots.length <= 1) {
    currentSlots[0] = '';
  } else {
    currentSlots.splice(idx, 1);
  }
  renderSlots();
}

function removeLastSlot() {
  if (currentSlots.length > 1) {
    currentSlots.pop();
    renderSlots();
  }
}

function resetBuiltinSlots() {
  currentSlots = [
    'builtin://1.mp3',
    'builtin://2.mp3',
    'builtin://3.mp3',
    'builtin://4.mp3',
    'builtin://5.mp3'
  ];
  renderSlots();
}

async function onLangChange() {
  applyI18n();
  await saveConfig();
}

async function loadState() {
  const r = await fetch('/api/state');
  const d = await r.json();
  lastState = d;
  for (const k of ['exit_mode','exit_mp3_path','idle_mode','idle_mp3_path',
                   'volume','watch_processes','min_process_ms','idle_seconds','language']) {
    const el = document.getElementById(k);
    if (el && d[k] !== undefined) el.value = d[k];
  }
  currentSlots = Array.isArray(d.slots) && d.slots.length > 0 ? d.slots.slice() : ['', '', '', '', ''];
  document.getElementById('volText').textContent = d.volume;
  document.getElementById('watch_enabled').checked = !!d.watch_enabled;
  document.getElementById('autostart').checked = !!d.autostart;
  applyI18n();
}

async function saveConfig() {
  const params = new URLSearchParams({
    language: document.getElementById('language').value,
    exit_mode: document.getElementById('exit_mode').value,
    exit_mp3_path: document.getElementById('exit_mp3_path').value,
    idle_mode: document.getElementById('idle_mode').value,
    idle_mp3_path: document.getElementById('idle_mp3_path').value,
    slot_count: String(currentSlots.length),
    volume: document.getElementById('volume').value,
    watch_enabled: document.getElementById('watch_enabled').checked ? 'true' : 'false',
    watch_processes: document.getElementById('watch_processes').value,
    min_process_ms: document.getElementById('min_process_ms').value,
    idle_seconds: document.getElementById('idle_seconds').value,
    autostart: document.getElementById('autostart').checked ? 'true' : 'false'
  });
  currentSlots.forEach((p, i) => params.set(`slot_${i}`, p || ''));
  const r = await fetch('/api/save', { method: 'POST', body: params });
  const d = await r.json();
  const t = tr();
  showToast(d.ok ? t.savedOk : (t.saveFail + (d.error || '')));
  await loadState();
}

async function pickSingleFile(targetId) {
  showToast(tr().pickPrompt);
  const r = await fetch('/api/pick?kind=mp3');
  const d = await r.json();
  if (d.path) {
    document.getElementById(targetId).value = d.path;
    document.getElementById(targetId.startsWith('exit') ? 'exit_mode' : 'idle_mode').value = 'mp3';
    await saveConfig();
  }
}

async function pickSlotFile(idx) {
  showToast(tr().pickSlotPrompt(idx + 1));
  const r = await fetch('/api/pick?kind=mp3');
  const d = await r.json();
  if (d.path) {
    currentSlots[idx] = d.path;
    renderSlots();
    await saveConfig();
  }
}

async function testSlot(idx) {
  const p = currentSlots[idx] || '';
  await fetch('/api/play?which=slot&path=' + encodeURIComponent(p), { method: 'POST' });
}

async function testPlay(which) {
  await fetch('/api/play?which=' + encodeURIComponent(which), { method: 'POST' });
}

async function stopPlay() {
  await fetch('/api/stop', { method: 'POST' });
}

loadState();
</script>
</body>
</html>
)HTML";

struct DaemonState {
    std::mutex mu;
    AppConfig cfg;
    AudioEngine audio;
    std::atomic<bool> running{true};
    SOCKET ipc_listen = INVALID_SOCKET;
    SOCKET web_listen = INVALID_SOCKET;
};

std::string build_state_json(DaemonState& st) {
    AppConfig cfg_copy;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        cfg_copy = st.cfg;
    }
    std::ostringstream oss;
    oss << "{"
        << "\"exit_mode\":\"" << json_escape(cfg_copy.exit_sound.mode) << "\","
        << "\"exit_mp3_path\":\"" << json_escape(cfg_copy.exit_sound.mp3_path) << "\","
        << "\"idle_mode\":\"" << json_escape(cfg_copy.idle_sound.mode) << "\","
        << "\"idle_mp3_path\":\"" << json_escape(cfg_copy.idle_sound.mp3_path) << "\","
        << "\"slots\":[";
    for (size_t i = 0; i < cfg_copy.random_slots.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << json_escape(cfg_copy.random_slots[i]) << "\"";
    }
    oss << "],"
        << "\"language\":\"" << json_escape(cfg_copy.language) << "\","
        << "\"resolved_lang\":\"" << (is_zh_language(cfg_copy.language) ? "zh" : "en") << "\","
        << "\"volume\":" << cfg_copy.volume << ","
        << "\"watch_enabled\":" << (cfg_copy.watch_enabled ? "true" : "false") << ","
        << "\"watch_processes\":\"" << json_escape(cfg_copy.watch_processes) << "\","
        << "\"min_process_ms\":" << cfg_copy.min_process_ms << ","
        << "\"idle_seconds\":" << cfg_copy.idle_seconds << ","
        << "\"autostart\":" << (is_autostart_enabled() ? "true" : "false") << ","
        << "\"backend\":\"" << json_escape(st.audio.backend_info()) << "\","
        << "\"cached_count\":" << st.audio.cached_mp3_count() << ","
        << "\"cached_bytes\":" << st.audio.cached_mp3_bytes() << ","
        << "\"last_error\":\"" << json_escape(st.audio.last_error()) << "\""
        << "}";
    return oss.str();
}

void reload_daemon_config(DaemonState& st) {
    AppConfig loaded = load_config();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        loaded.ipc_port = st.cfg.ipc_port;
        loaded.web_port = st.cfg.web_port;
        st.cfg = loaded;
    }
    st.audio.sync_config(loaded);
}

void handle_http_client(SOCKET client, DaemonState& st) {
    std::string req;
    char buf[4096];
    int content_len = 0;
    size_t hdr_end = std::string::npos;

    while (req.size() < 262144) {
        int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
        if (hdr_end == std::string::npos) {
            hdr_end = req.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                std::string headers = to_lower_ascii(req.substr(0, hdr_end));
                auto cl_pos = headers.find("content-length:");
                if (cl_pos != std::string::npos) {
                    try {
                        content_len = std::stoi(headers.substr(cl_pos + 15));
                    } catch (...) {}
                }
            }
        }
        if (hdr_end != std::string::npos && req.size() >= hdr_end + 4 + static_cast<size_t>(std::max(0, content_len))) {
            break;
        }
    }

    if (hdr_end == std::string::npos) {
        closesocket(client);
        return;
    }

    std::string first_line = req.substr(0, req.find("\r\n"));
    std::istringstream iss(first_line);
    std::string method, target;
    iss >> method >> target;
    std::string body = req.substr(hdr_end + 4);

    std::string path = target;
    std::string query;
    auto qpos = target.find('?');
    if (qpos != std::string::npos) {
        path = target.substr(0, qpos);
        query = target.substr(qpos + 1);
    }

    auto send_resp = [&](const std::string& status, const std::string& ctype, const std::string& payload) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << "\r\n"
            << "Content-Type: " << ctype << "\r\n"
            << "Content-Length: " << payload.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Connection: close\r\n\r\n"
            << payload;
        send_all(client, oss.str());
    };

    if (path == "/" || path == "/index.html") {
        send_resp("200 OK", "text/html; charset=utf-8", kWebUiHtml);
    } else if (path == "/api/state") {
        send_resp("200 OK", "application/json; charset=utf-8", build_state_json(st));
    } else if (path == "/api/play" && method == "POST") {
        auto params = parse_form_urlencoded(query);
        AppConfig cfg_copy;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            cfg_copy = st.cfg;
        }
        if (params["which"] == "slot") {
            st.audio.play_mp3_path(params["path"], cfg_copy.volume);
        } else if (params["which"] == "idle") {
            st.audio.play_idle(cfg_copy);
        } else {
            st.audio.play_exit(cfg_copy);
        }
        send_resp("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
    } else if (path == "/api/stop" && method == "POST") {
        st.audio.stop();
        send_resp("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
    } else if (path == "/api/pick") {
        bool zh = false;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            zh = is_zh_language(st.cfg.language);
        }
        std::string picked = pick_file_dialog(
            zh ? "选择 MP3/音频文件 (将读取并常驻内存)" : "Select MP3 / Audio File (Cached in RAM)", "mp3");
        send_resp("200 OK", "application/json; charset=utf-8",
                  "{\"path\":\"" + json_escape(picked) + "\"}");
    } else if (path == "/api/save" && method == "POST") {
        auto form = parse_form_urlencoded(body);
        auto norm_mode = [](const std::string& m) -> std::string {
            return (m == "mp3" || m == "random") ? m : "ding";
        };
        AppConfig updated;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            updated = st.cfg;
            if (form.count("language")) updated.language = normalize_language(form["language"]);
            if (form.count("exit_mode")) updated.exit_sound.mode = norm_mode(form["exit_mode"]);
            if (form.count("exit_mp3_path")) updated.exit_sound.mp3_path = form["exit_mp3_path"];
            if (form.count("idle_mode")) updated.idle_sound.mode = norm_mode(form["idle_mode"]);
            if (form.count("idle_mp3_path")) updated.idle_sound.mp3_path = form["idle_mp3_path"];
            if (form.count("slot_count")) {
                try {
                    int cnt = std::clamp(std::stoi(form["slot_count"]), 1, 100);
                    updated.random_slots.assign(static_cast<size_t>(cnt), "");
                    for (int i = 0; i < cnt; ++i) {
                        std::string k = "slot_" + std::to_string(i);
                        if (form.count(k)) updated.random_slots[static_cast<size_t>(i)] = trim_copy(form[k]);
                    }
                } catch (...) {}
            }
            if (form.count("volume")) {
                try { updated.volume = std::clamp(std::stoi(form["volume"]), 0, 100); } catch (...) {}
            }
            if (form.count("watch_enabled")) updated.watch_enabled = (form["watch_enabled"] == "true");
            if (form.count("watch_processes")) updated.watch_processes = form["watch_processes"];
            if (form.count("min_process_ms")) {
                try { updated.min_process_ms = std::clamp(std::stoi(form["min_process_ms"]), 0, 600000); } catch (...) {}
            }
            if (form.count("idle_seconds")) {
                try { updated.idle_seconds = std::clamp(std::stoi(form["idle_seconds"]), 1, 86400); } catch (...) {}
            }
            st.cfg = updated;
        }
        save_config(updated);
        st.audio.sync_config(updated);

        std::string err;
        bool ok = true;
        if (form.count("autostart")) {
            bool want_auto = (form["autostart"] == "true");
            if (want_auto != is_autostart_enabled()) {
                ok = set_autostart(want_auto, err);
            }
        }
        if (ok) {
            send_resp("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        } else {
            send_resp("200 OK", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}");
        }
    } else {
        send_resp("404 Not Found", "text/plain; charset=utf-8", "Not Found");
    }
    closesocket(client);
}

void ipc_server_loop(DaemonState& st) {
    while (st.running.load(std::memory_order_acquire)) {
        SOCKET c = accept(st.ipc_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;

        char buf[1024];
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string cmd = trim_copy(std::string(buf, static_cast<size_t>(n)));
            std::string reply = "OK\n";

            AppConfig cfg_copy;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                cfg_copy = st.cfg;
            }

            if (cmd == "PING") {
                reply = "PONG\n";
            } else if (cmd == "PLAY_EXIT") {
                st.audio.play_exit(cfg_copy);
            } else if (cmd == "PLAY_IDLE") {
                st.audio.play_idle(cfg_copy);
            } else if (cmd == "STOP") {
                st.audio.stop();
            } else if (cmd == "RELOAD") {
                reload_daemon_config(st);
            } else if (cmd == "STATUS") {
                reply = build_state_json(st) + "\n";
            } else if (cmd == "QUIT") {
                reply = "BYE\n";
                send_all(c, reply);
                closesocket(c);
                st.running.store(false, std::memory_order_release);
                request_tray_quit();
                break;
            } else {
                reply = "ERR unknown command\n";
            }
            send_all(c, reply);
        }
        closesocket(c);
    }
}

void web_server_loop(DaemonState& st) {
    while (st.running.load(std::memory_order_acquire)) {
        SOCKET c = accept(st.web_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::thread([c, &st]() { handle_http_client(c, st); }).detach();
    }
}

void process_watch_loop(DaemonState& st) {
    struct SeenProc {
        uint64_t start_key = 0;
        std::chrono::steady_clock::time_point first_seen;
        uint64_t last_age_ms = 0;
    };

    std::unordered_map<uint64_t, SeenProc> active;
    bool initialized = false;

    while (st.running.load(std::memory_order_acquire)) {
        AppConfig cfg_copy;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            cfg_copy = st.cfg;
        }

        if (cfg_copy.watch_enabled) {
            auto names = split_csv(cfg_copy.watch_processes);
            std::unordered_set<std::string> watch_set;
            for (const auto& n : names) watch_set.insert(to_lower_ascii(n));

            auto procs = list_processes();
            auto now = std::chrono::steady_clock::now();
            std::unordered_map<uint64_t, SeenProc> next_active;

            for (const auto& p : procs) {
                if (watch_set.find(p.name) == watch_set.end()) continue;
                auto it = active.find(p.pid);
                if (it != active.end() && it->second.start_key == p.start_key) {
                    SeenProc sp = it->second;
                    sp.last_age_ms = std::max(sp.last_age_ms, p.age_ms);
                    next_active[p.pid] = sp;
                } else {
                    next_active[p.pid] = {p.start_key, now, p.age_ms};
                }
            }

            if (initialized) {
                bool any_exited = false;
                for (const auto& kv : active) {
                    if (next_active.find(kv.first) == next_active.end()) {
                        uint64_t observed_ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - kv.second.first_seen).count());
                        uint64_t lifetime_ms = std::max(kv.second.last_age_ms, observed_ms);
                        if (lifetime_ms >= static_cast<uint64_t>(cfg_copy.min_process_ms)) {
                            any_exited = true;
                            break;
                        }
                    }
                }
                if (any_exited) {
                    st.audio.play_exit(cfg_copy);
                }
            }

            active = std::move(next_active);
            initialized = true;
        } else {
            active.clear();
            initialized = false;
        }

        int sleep_ms = std::clamp(cfg_copy.poll_interval_ms, 100, 5000);
        for (int elapsed = 0; elapsed < sleep_ms && st.running.load(std::memory_order_acquire); elapsed += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

} // namespace

bool send_ipc_command(int port, const std::string& cmd, std::string& reply) {
    int target_port = port;
    int rt_ipc = 0, rt_web = 0;
    if (read_runtime_ports(rt_ipc, rt_web) && rt_ipc > 0) {
        target_port = rt_ipc;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;

    auto set_nonblock = [&](bool nb) {
#ifdef _WIN32
        u_long mode = nb ? 1UL : 0UL;
        ioctlsocket(s, FIONBIO, &mode);
#else
        int flags = fcntl(s, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
        }
#endif
    };

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(target_port));

    set_nonblock(true);
    int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv{0, 200000}; // 200ms connect timeout
        int sel = select(static_cast<int>(s) + 1, nullptr, &wfds, nullptr, &tv);
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (sel <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len) != 0 || so_err != 0) {
            closesocket(s);
            return false;
        }
    }
    set_nonblock(false);

#ifdef _WIN32
    DWORD timeout_ms = 600;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{0, 600000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    std::string msg = cmd + "\n";
    send_all(s, msg);

    reply.clear();
    char buf[4096];
    int n = 0;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        reply.append(buf, static_cast<size_t>(n));
        if (reply.find('\n') != std::string::npos) break;
    }
    closesocket(s);
    reply = trim_copy(reply);
    return !reply.empty();
}

int run_daemon(bool open_ui_on_start) {
    AppConfig cfg = load_config();

    int rt_ipc = 0, rt_web = 0;
    if (read_runtime_ports(rt_ipc, rt_web)) {
        std::string pong;
        if (send_ipc_command(rt_ipc, "PING", pong) && pong == "PONG") {
            if (open_ui_on_start) {
                open_url_in_browser("http://127.0.0.1:" + std::to_string(rt_web));
            }
            return 0;
        }
        remove_runtime_ports();
    }

    DaemonState st;
    st.cfg = cfg;
    st.audio.init();
    st.audio.sync_config(cfg);

    int active_ipc_port = 0;
    int active_web_port = 0;
    st.ipc_listen = create_loopback_listener(cfg.ipc_port, active_ipc_port);
    if (st.ipc_listen == INVALID_SOCKET) {
        std::cerr << "Failed to bind IPC port\n";
        return 1;
    }
    st.web_listen = create_loopback_listener(cfg.web_port, active_web_port);
    st.cfg.ipc_port = active_ipc_port;
    st.cfg.web_port = active_web_port;
    write_runtime_ports(active_ipc_port, active_web_port);

    std::thread ipc_thread([&]() { ipc_server_loop(st); });
    std::thread web_thread;
    if (st.web_listen != INVALID_SOCKET) {
        web_thread = std::thread([&]() { web_server_loop(st); });
    }
    std::thread watch_thread([&]() { process_watch_loop(st); });

    if (open_ui_on_start && st.web_listen != INVALID_SOCKET) {
        open_url_in_browser("http://127.0.0.1:" + std::to_string(active_web_port));
    }

    TrayCallbacks cb;
    cb.on_open_settings = [&]() {
        open_url_in_browser("http://127.0.0.1:" + std::to_string(st.cfg.web_port));
    };
    cb.on_pick_exit_mp3 = [&]() {
        bool zh = false;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            zh = is_zh_language(st.cfg.language);
        }
        std::string p = pick_file_dialog(
            zh ? "选择批处理/终端退出 MP3 (将常驻内存)" : "Select Exit MP3 (Cached in RAM)", "mp3");
        if (!p.empty()) {
            AppConfig c;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                st.cfg.exit_sound.mode = "mp3";
                st.cfg.exit_sound.mp3_path = p;
                c = st.cfg;
            }
            save_config(c);
            st.audio.sync_config(c);
            st.audio.play_exit(c);
        }
    };
    cb.on_pick_idle_mp3 = [&]() {
        bool zh = false;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            zh = is_zh_language(st.cfg.language);
        }
        std::string p = pick_file_dialog(
            zh ? "选择终端闲置 MP3 (将常驻内存)" : "Select Idle MP3 (Cached in RAM)", "mp3");
        if (!p.empty()) {
            AppConfig c;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                st.cfg.idle_sound.mode = "mp3";
                st.cfg.idle_sound.mp3_path = p;
                c = st.cfg;
            }
            save_config(c);
            st.audio.sync_config(c);
            st.audio.play_idle(c);
        }
    };
    cb.on_test_exit = [&]() {
        AppConfig c;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            c = st.cfg;
        }
        st.audio.play_exit(c);
    };
    cb.on_test_idle = [&]() {
        AppConfig c;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            c = st.cfg;
        }
        st.audio.play_idle(c);
    };
    cb.is_watch_enabled = [&]() {
        std::lock_guard<std::mutex> lk(st.mu);
        return st.cfg.watch_enabled;
    };
    cb.on_toggle_watch = [&]() {
        AppConfig c;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            st.cfg.watch_enabled = !st.cfg.watch_enabled;
            c = st.cfg;
        }
        save_config(c);
    };
    cb.is_autostart_enabled = []() { return is_autostart_enabled(); };
    cb.on_toggle_autostart = []() {
        std::string err;
        set_autostart(!is_autostart_enabled(), err);
    };
    cb.is_zh = [&]() {
        std::lock_guard<std::mutex> lk(st.mu);
        return is_zh_language(st.cfg.language);
    };
    cb.on_quit = [&]() {
        st.running.store(false, std::memory_order_release);
    };

    run_tray_or_wait_loop(cb);

    st.running.store(false, std::memory_order_release);
    if (st.ipc_listen != INVALID_SOCKET) {
        closesocket(st.ipc_listen);
        st.ipc_listen = INVALID_SOCKET;
    }
    if (st.web_listen != INVALID_SOCKET) {
        closesocket(st.web_listen);
        st.web_listen = INVALID_SOCKET;
    }

    if (watch_thread.joinable()) watch_thread.join();
    if (ipc_thread.joinable()) ipc_thread.join();
    if (web_thread.joinable()) web_thread.join();
    st.audio.shutdown();
    remove_runtime_ports();
    return 0;
}

} // namespace aoh
