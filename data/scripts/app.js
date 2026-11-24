"use strict";

function $(id) { return document.getElementById(id); }
function setMsg(id, text, ok) {
  var el = $(id); if (!el) return;
  el.textContent = text;
  el.style.color = ok ? "#2e7d32" : "#d32f2f";
}
// Any 401 from the device means the session expired -> back to login.
function guard(r) { if (r.status === 401) { window.location.href = "/"; throw new Error("unauth"); } return r; }

// ---------- tab switching ----------
function showContent(menuId) {
  var sections = document.querySelectorAll(".content-div");
  for (var i = 0; i < sections.length; i++) {
    var on = sections[i].id === menuId;
    sections[i].style.display = on ? "block" : "none";
    sections[i].classList.toggle("active", on);
  }
  var menus = document.querySelectorAll(".main_menu");
  for (var j = 0; j < menus.length; j++) menus[j].classList.remove("active");
  var active = document.querySelector(".menu-" + menuId.split("-")[1]);
  if (active) active.classList.add("active");
  // Fresh data as soon as a tab opens (functions are defined further down).
  if (menuId === "menu-1" && typeof loadHomeLog === "function") loadHomeLog();
  if (menuId === "menu-3" && typeof loadProtection === "function") loadProtection();
}

function viewPassword(id) {
  var el = $(id);
  el.type = el.type === "password" ? "text" : "password";
}

// ---------- live readings (3-phase V/I/P/E) ----------
function n1(x) { return Number(x).toFixed(1); }
function n2(x) { return Number(x).toFixed(2); }
function n3(x) { return Number(x).toFixed(3); }

function applyReadings(d) {
  $("v1").textContent = n1(d.v1); $("v2").textContent = n1(d.v2); $("v3").textContent = n1(d.v3);
  $("i1").textContent = n2(d.i1); $("i2").textContent = n2(d.i2); $("i3").textContent = n2(d.i3);
  $("p1").textContent = n1(d.p1); $("p2").textContent = n1(d.p2); $("p3").textContent = n1(d.p3);
  $("e1").textContent = n3(d.e1); $("e2").textContent = n3(d.e2); $("e3").textContent = n3(d.e3);
  // Cumulative (lifetime, flash-backed) energy per phase.
  $("c1").textContent = n3(d.c1); $("c2").textContent = n3(d.c2); $("c3").textContent = n3(d.c3);

  // Inverter (4th channel). Not included in the totals below.
  if ($("v4")) {
    $("v4").textContent = n1(d.v4); $("i4").textContent = n2(d.i4);
    $("p4").textContent = n1(d.p4); $("e4").textContent = n3(d.e4);
    $("c4").textContent = n3(d.c4);
  }

  // Totals: power summed live; Total Energy = sum of the phases' cumulative energy.
  $("pT").textContent = n1(Number(d.p1) + Number(d.p2) + Number(d.p3));
  $("eT").textContent = n3(Number(d.c1) + Number(d.c2) + Number(d.c3));

  var t = (d.stmTime && d.stmTime.length) ? d.stmTime : "—";
  // Home tab and Settings > Device Time both show the STM32 RTC time.
  $("systemTime").textContent = t;
  if ($("rtcTime")) $("rtcTime").textContent = t;
  if ($("battV")) $("battV").textContent = n1(d.batt);
  if ($("batteryChip")) $("batteryChip").classList.toggle("low", Number(d.batt) < 1.5);

  // Power-flow arrows (relayStatus bits 0-9) and the UPS aux badge (bit 10).
  drawFlows(Number(d.relayStatus) || 0);

  // Contactor feedback: d.cf bit0 = CF1 ... bit6 = CF7 (1 = ON).
  var cf = Number(d.cf) || 0;
  for (var c = 0; c < 7; c++) {
    var chip = $("cf" + (c + 1));
    if (!chip) continue;
    var on = (cf >> c) & 1;
    chip.classList.toggle("on", !!on);
    chip.querySelector(".cs").textContent = on ? "ON" : "OFF";
  }
}

// ---------- power-flow arrows (Home tab SVG) ----------
// relayStatus bit -> arrows it shows. Arrow id flSO: source S (0=L1 1=L2 2=L3 3=UPS) feeds
// output O (0=R 1=Y 2=B). Aux relay xyAux = line x feeds output y (STM main.c): e.g. when
// R fails the STM turns on yrAux, so the Y line arrow must go to the R output.
var FLOWS = [
  ["fl00"],                  // 0  rMain  : L1 -> R
  ["fl11"],                  // 1  yMain  : L2 -> Y
  ["fl22"],                  // 2  bMain  : L3 -> B
  ["fl01"],                  // 3  ryAux  : L1 (R) -> Y output   (Y failed)
  ["fl10"],                  // 4  yrAux  : L2 (Y) -> R output   (R failed)
  ["fl12"],                  // 5  ybAux  : L2 (Y) -> B output   (B failed)
  ["fl21"],                  // 6  byAux  : L3 (B) -> Y output   (Y failed)
  ["fl02"],                  // 7  rbAux  : L1 (R) -> B output   (B failed)
  ["fl20"],                  // 8  brAux  : L3 (B) -> R output   (R failed)
  ["fl30", "fl31", "fl32"]   // 9  upsMain: UPS -> R, Y, B
];

function drawFlows(rs) {
  for (var b = 0; b < FLOWS.length; b++) {
    for (var k = 0; k < FLOWS[b].length; k++) {
      var el = $(FLOWS[b][k]);
      if (el) el.classList.toggle("off", !((rs >> b) & 1));
    }
  }
}

// ---------- live data over WebSocket (port 81) ----------
// Readings, WiFi status and cloud status are pushed by the device (src/ws_events.cpp).
// There is NO /api/getReadings endpoint any more — do not bring back fetch polling for them.
// The event log and protection settings use the HTTP APIs (loadHomeLog / loadProtection).
var LIVE_STALE_MS = 8000;   // device pushes readings at least every 2 s
var live = { ws: null, retry: 1000, lastMsg: 0, timer: null };

function sessionId() {
  var m = document.cookie.match(/(?:^|;\s*)eviqsid=(\d+)/);
  return m ? m[1] : "";
}

function scheduleLive() {
  if (live.timer) return;
  live.timer = setTimeout(function () { live.timer = null; connectLive(); }, live.retry);
  live.retry = Math.min(live.retry * 2, 10000);
}

function dropLive() {
  var ws = live.ws;
  live.ws = null;
  if (ws) {
    ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
    try { ws.close(); } catch (e) {}
  }
  scheduleLive();
}

function connectLive() {
  if (!sessionId()) { window.location.href = "/"; return; }
  var ws;
  try { ws = new WebSocket("ws://" + location.hostname + ":81/live?sid=" + sessionId()); }
  catch (e) { scheduleLive(); return; }
  live.ws = ws;
  ws.onopen = function () { live.retry = 1000; live.lastMsg = Date.now(); };
  ws.onmessage = function (e) {
    live.lastMsg = Date.now();
    var m;
    try { m = JSON.parse(e.data); } catch (x) { return; }
    onLiveMessage(m);
  };
  ws.onclose = function () { if (live.ws === ws) dropLive(); };
  ws.onerror = function () { if (live.ws === ws) dropLive(); };
}

function onLiveMessage(m) {
  switch (m.type) {
    case "readings": applyReadings(m); break;
    case "wifi":     applyWiFiStatus(m); break;
    case "cloud":    applyCloudStatus(m); break;
    case "auth":     window.location.href = "/"; break;   // session expired / logged out
  }
}

// A silently dead link (WiFi drop) never fires onclose quickly: detect staleness ourselves.
setInterval(function () {
  if (live.ws && live.ws.readyState === 1 && Date.now() - live.lastMsg > LIVE_STALE_MS) dropLive();
}, 2000);

// ---------- wifi status / header ----------
function setWifiIcon(mode, rssi) {
  ["wifi-e", "wifi-g", "wifi-w", "apIcon"].forEach(function (id) { $(id).style.display = "none"; });
  if (mode === "WIFI") {
    var id = rssi >= -60 ? "wifi-e" : (rssi >= -75 ? "wifi-g" : "wifi-w");
    $(id).style.display = "";
  } else {
    $("apIcon").style.display = "";
  }
}

function applyWiFiStatus(s) {
  $("switchNameText").textContent = s.deviceName;
  document.title = s.deviceName + " — Meter";
  if (!$("deviceName").value) $("deviceName").value = s.deviceName;
  if (!$("ssid").value && s.savedSSID) $("ssid").value = s.savedSSID;

  $("apwifiName").textContent = s.ssid || "";
  setWifiIcon(s.mode, s.rssi);

  if (s.mode === "WIFI") {
    var sig = s.rssi >= -60 ? "Strong" : (s.rssi >= -75 ? "Good" : "Weak");
    $("connectionDetails").textContent = "Connected to \"" + s.ssid + "\" (signal: " + sig + ").";
    $("wiFiDetails").textContent = "Open this page at  http://" + s.mdns + "  or  http://" + s.ip;
  } else {
    $("connectionDetails").textContent = "Running in Access Point mode (" + s.ssid + ").";
    $("wiFiDetails").textContent = "Enter your WiFi below to connect to your home network.";
  }
  if (s.mac) $("macDetails").textContent = "Device MAC address: " + s.mac;
}

// ---------- wifi scan ----------
function scanNetworks() {
  var box = $("scanList");
  box.textContent = "Scanning…";
  var attempts = 0;
  function poll() {
    fetch("/api/scanNetworks").then(guard)
      .then(function (r) { return r.json(); })
      .then(function (res) {
        if (res.status === "scanning" && attempts++ < 15) { setTimeout(poll, 1000); return; }
        if (!res.networks || !res.networks.length) { box.textContent = "No networks found."; return; }
        box.innerHTML = "";
        res.networks.forEach(function (n) {
          var a = document.createElement("a");
          a.href = "#";
          a.style.cssText = "display:block; padding:4px 0; color:#1565c0; text-decoration:none;";
          a.textContent = n.ssid + "  (" + n.rssi + " dBm)" + (n.secure ? " 🔒" : "");
          a.onclick = function (e) { e.preventDefault(); $("ssid").value = n.ssid; $("pass").focus(); };
          box.appendChild(a);
        });
      })
      .catch(function () { box.textContent = "Scan failed."; });
  }
  poll();
}

// ---------- settings actions ----------
function postWithPin(url, obj, msgId, okText, after) {
  var pin = window.prompt("Enter your device PIN to save changes:");
  if (pin === null) return;
  if (!/^\d{4}$/.test(pin)) { setMsg(msgId, "Enter your 4-digit PIN.", false); return; }
  obj.authPin = pin;
  setMsg(msgId, "Saving…", true);
  fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(obj) })
    .then(guard)
    .then(function (r) { return r.text().then(function (t) { setMsg(msgId, t || okText, r.ok); if (r.ok && after) after(); }); })
    .catch(function () { setMsg(msgId, "Request failed.", false); });
}

function saveWifi() {
  var ssid = $("ssid").value.trim();
  if (!ssid) { setMsg("wifiMsg", "Enter a WiFi name.", false); return; }
  postWithPin("/api/setWiFi", { ssid: ssid, pass: $("pass").value }, "wifiMsg", "Saved.");
}
function saveName() {
  var name = $("deviceName").value.trim();
  if (name.length < 4) { setMsg("nameMsg", "Name must be 4–20 characters.", false); return; }
  postWithPin("/api/setDeviceName", { name: name }, "nameMsg", "Saved.");
}
function saveApPass() {
  var p = $("APPass").value;
  if (p.length > 0 && p.length < 8) { setMsg("apMsg", "AP password must be ≥ 8 characters (or blank).", false); return; }
  postWithPin("/api/setAPPassword", { pass: p }, "apMsg", "Saved. Restart to apply.");
}
function savePin() {
  var pin = $("devicePin").value;
  if (!/^\d{4}$/.test(pin)) { setMsg("pinMsg", "New PIN must be 4 digits.", false); return; }
  postWithPin("/api/setPin", { pin: pin }, "pinMsg", "PIN updated.");
}
function disconnectWifi() {
  if (!confirm("Forget the saved WiFi and switch to Access Point mode?")) return;
  postWithPin("/api/disableWiFi", {}, "wifiMsg", "Disconnected.", function () {
    $("ssid").value = "";
  });
}
function logout() {
  var pin = window.prompt("Enter your device PIN to log out:");
  if (pin === null) return;
  if (!/^\d{4}$/.test(pin)) { alert("Enter your 4-digit PIN."); return; }
  fetch("/api/logout", {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ authPin: pin })
  }).then(function (r) {
    if (r.ok) { window.location.href = "/"; }
    else { r.text().then(function (t) { alert(t || "Incorrect PIN."); }); }
  }).catch(function () { alert("Request failed."); });
}

// ---------- cloud (MQTT) ----------
function applyCloudStatus(c) {
  $("cloudStatusText").textContent = c.message || "—";
  if (c.configured) {
    $("cloudEmailDiv").style.display = "none";
    $("cloudStatusDiv").style.display = "block";
    if (c.email && !$("clEmail").value) $("clEmail").value = c.email;
  } else {
    $("cloudEmailDiv").style.display = "block";
    $("cloudStatusDiv").style.display = "none";
  }
}

// Status updates after these actions arrive by themselves over the WebSocket.
function connectCloud() {
  var email = $("clEmail").value.trim();
  if (email.length < 5 || email.indexOf("@") < 0) { setMsg("cloudMsg", "Enter a valid email.", false); return; }
  postWithPin("/api/connectCloud", { email: email }, "cloudMsg", "Registering…");
}

function disconnectCloud() {
  if (!confirm("Disconnect this device from the EVIQtech cloud?")) return;
  postWithPin("/api/pauseCloud", { operation: "disconnect" }, "cloudMsg", "Disconnected.");
}

// ---------- output voltage on a failed phase ----------
// When a phase input is off, the STM feeds that phase's output through an aux relay
// from another phase. relayStatus (d.rs) bits from the STM frame:
//   3 = R->Y aux, 4 = Y->R aux, 5 = Y->B aux, 6 = B->Y aux, 7 = R->B aux, 8 = B->R aux.
// Feeders per phase box (1 = L1/R, 2 = L2/Y, 3 = L3/B): [relay bit, source phase].
var PHASE_OFF_V = 100;   // below this the input is treated as off (0 V or noise)
var OUT_FEEDERS = {
  1: [[4, 2], [8, 3]],   // R output fed from Y or B
  2: [[3, 1], [6, 3]],   // Y output fed from R or B
  3: [[7, 1], [5, 2]]    // B output fed from R or Y
};
var PHASE_LETTER = { 1: "R", 2: "Y", 3: "B", 4: "INV" };
var UPS_MAIN_BIT = 9;    // all phases down -> STM turns the UPS main relay ON, inverter feeds R, Y and B
function updateOutputVoltage(d) {
  var rs = Number(d.rs) || 0;
  var upsOn = (rs >> UPS_MAIN_BIT) & 1;
  for (var ph = 1; ph <= 3; ph++) {
    var cell = $("out" + ph);
    if (!cell) continue;
    var src = 0;
    if (upsOn) {
      src = 4;             // inverter output (v4) on every phase
    } else {
      var f = OUT_FEEDERS[ph];
      for (var k = 0; k < f.length; k++) {
        if ((rs >> f[k][0]) & 1) { src = f[k][1]; break; }
      }
    }
    var inputOff = Number(d["v" + ph]) < PHASE_OFF_V;
    var show = inputOff || src !== 0;   // only appears while this phase is down / switched over
    cell.classList.toggle("show", show);
    if (!show) continue;
    cell.querySelector(".pl").textContent = src ? ("Output (from " + PHASE_LETTER[src] + ")") : "Output (no source)";
    cell.querySelector(".pv").textContent = src ? n1(d["v" + src]) : "0";
  }
}

// ---------- protection ----------
var protLoaded = false;
function fillProt(i, p) {
  $("protEn" + i).checked = !!p.en;
  $("protHv" + i).value = p.hv;
  $("protLv" + i).value = p.lv;
  $("protHi" + i).value = p.hi;
  if ($("protHp" + i) && p.hp !== undefined) $("protHp" + i).value = p.hp;   // UPS only
}
function updProtStatus(i, p) {
  $("protLiveV" + i).textContent = Number(p.v).toFixed(1);
  $("protLiveI" + i).textContent = Number(p.i).toFixed(2);
  if ($("protLiveP" + i) && p.p !== undefined) $("protLiveP" + i).textContent = Number(p.p).toFixed(1);
  var s = $("protStat" + i);
  s.textContent = p.status;
  s.style.color = (p.status === "OK") ? "#1b7a4b"
                : (p.status === "off" || p.status === "—") ? "#999" : "#d32f2f";
}
var protBusy = false;
function loadProtection() {
  if (protBusy) return;
  protBusy = true;
  fetch("/api/getProtection").then(guard)
    .then(function (r) { return r.json(); })
    .then(function (res) {
      var ph = res.phases || [];
      for (var k = 0; k < ph.length; k++) {
        var i = k + 1;
        if (!$("protEn" + i)) continue;   // no box for this phase on the page
        if (!protLoaded) fillProt(i, ph[k]);   // don't clobber edits after first load
        updProtStatus(i, ph[k]);
      }
      protLoaded = true;
    })
    .catch(function () {})
    .then(function () { protBusy = false; });
}
// Auto-hiding toast (bottom-center). ok=true -> green, false -> red. Hides after 5 s.
function showToast(text, ok) {
  var t = $("toast");
  if (!t) { t = document.createElement("div"); t.id = "toast"; document.body.appendChild(t); }
  t.textContent = text;
  t.style.cssText = "position:fixed; left:50%; bottom:24px; transform:translateX(-50%);"
    + "background:" + (ok ? "#2e7d32" : "#d32f2f") + "; color:#fff; padding:11px 20px;"
    + "border-radius:8px; font-size:.85rem; max-width:90%; text-align:center; z-index:9999;"
    + "box-shadow:0 3px 10px rgba(0,0,0,.28); opacity:1; transition:opacity .4s;";
  t.style.display = "block";
  clearTimeout(t._timer);
  t._timer = setTimeout(function () {
    t.style.opacity = "0";
    setTimeout(function () { t.style.display = "none"; }, 400);
  }, 5000);   // disappears after 5 seconds
}

function saveProtection() {
  var phases = [];
  for (var i = 1; i <= 4; i++) {   // L1, L2, L3, Inverter
    var ph = {
      en: $("protEn" + i).checked,
      hv: parseFloat($("protHv" + i).value),
      lv: parseFloat($("protLv" + i).value),
      hi: parseFloat($("protHi" + i).value)
    };
    if (i === 4) ph.hp = parseFloat($("protHp4").value);   // UPS power limit
    phases.push(ph);
  }
  // Validate only the phases the user enabled. Any failure warns via toast (5 s) and stops.
  for (var j = 0; j < phases.length; j++) {
    var p = phases[j], L = (j === 3) ? "Inverter" : "L" + (j + 1);
    if (!p.en) continue;
    if (isNaN(p.hv) || isNaN(p.lv) || isNaN(p.hi)) {
      showToast(L + ": please fill High Voltage, Low Voltage and Overload Current.", false); return;
    }
    if (p.lv < 150 || p.lv > 250) {
      showToast(L + ": Low Voltage must be between 150 and 250 V.", false); return;
    }
    if (p.hv < 200 || p.hv > 290) {
      showToast(L + ": High Voltage must be between 200 and 290 V.", false); return;
    }
    if (p.hv <= p.lv) {
      showToast(L + ": High Voltage must be higher than Low Voltage.", false); return;
    }
    if (p.hi < 0.01 || p.hi > 50) {
      showToast(L + ": Overload Current must be between 0.01 and 50 A.", false); return;
    }
    if (j === 3 && (isNaN(p.hp) || p.hp < 1 || p.hp > 10000)) {
      showToast(L + ": Power Limit must be between 1 and 10000 W.", false); return;
    }
  }

  var pin = window.prompt("Enter your device PIN to save changes:");
  if (pin === null) return;
  if (!/^\d{4}$/.test(pin)) { showToast("Enter your 4-digit PIN.", false); return; }
  fetch("/api/setProtection", {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ phases: phases, authPin: pin })
  }).then(guard)
    .then(function (r) { return r.text().then(function (t) {
      showToast(t || (r.ok ? "Protection settings saved." : "Save failed."), r.ok);
      if (r.ok) loadProtection();
    }); })
    .catch(function () { showToast("Request failed.", false); });
}

// ---------- firmware OTA ----------
function firmwareUpdate(e) {
  e.preventDefault();
  var f = $("firmware").files[0];
  if (!f) { setMsg("otaMsg", "Choose a .bin file.", false); return false; }
  var pin = window.prompt("Enter your device PIN to update the firmware:");
  if (pin === null) return false;
  if (!/^\d{4}$/.test(pin)) { setMsg("otaMsg", "Enter your 4-digit PIN.", false); return false; }
  setMsg("otaMsg", "Uploading… do not power off.", true);
  var fd = new FormData();
  fd.append("firmware", f);
  fetch("/updateSystem?authPin=" + encodeURIComponent(pin), { method: "POST", body: fd })
    .then(guard)
    .then(function (r) { return r.text().then(function (t) {
      if (r.ok) {
        setMsg("otaMsg", "Update complete. Rebooting…", true);
        setTimeout(function () { window.location.href = "/"; }, 12000);
      } else { setMsg("otaMsg", t || "Update failed.", false); }
    }); })
    .catch(function () { setMsg("otaMsg", "Upload failed.", false); });
  return false;
}

// ---------- STM32 front-end OTA ----------
// Unlike the ESP update, the device does NOT reboot afterwards — only the STM32
// does — so on success we just report and let metering resume.
function stm32Update(e) {
  e.preventDefault();
  var f = $("stmFirmware").files[0];
  if (!f) { setMsg("stmOtaMsg", "Choose a .bin file.", false); return false; }
  var pin = window.prompt("Enter your device PIN to update the front-end:");
  if (pin === null) return false;
  if (!/^\d{4}$/.test(pin)) { setMsg("stmOtaMsg", "Enter your 4-digit PIN.", false); return false; }
  setMsg("stmOtaMsg", "Uploading, then flashing front-end… this can take a minute.", true);
  var fd = new FormData();
  fd.append("firmware", f);
  fetch("/updateSTM32?authPin=" + encodeURIComponent(pin), { method: "POST", body: fd })
    .then(guard)
    .then(function (r) { return r.text().then(function (t) {
      if (r.ok) { setMsg("stmOtaMsg", "Sent. Press \"Install & Restart\" to apply it.", true); }
      else { setMsg("stmOtaMsg", t || "Front-end update failed.", false); }
    }); })
    .catch(function () { setMsg("stmOtaMsg", "Upload failed.", false); });
  return false;
}

// Stage 2: install the staged image. The front-end reboots into its bootloader,
// copies bank 2 over the application, and restarts.
function stm32Apply() {
  if (!window.confirm("Install the firmware already sent to the front-end?\n\nMetering stops for a few seconds while it restarts.")) return;
  var pin = window.prompt("Enter your device PIN to install:");
  if (pin === null) return;
  if (!/^\d{4}$/.test(pin)) { setMsg("stmOtaMsg", "Enter your 4-digit PIN.", false); return; }
  setMsg("stmOtaMsg", "Installing… front-end is restarting.", true);
  fetch("/applySTM32?authPin=" + encodeURIComponent(pin), { method: "POST" })
    .then(guard)
    .then(function (r) { return r.text().then(function (t) {
      if (r.ok) { setMsg("stmOtaMsg", "Installed. Front-end restarted.", true); }
      else { setMsg("stmOtaMsg", t || "Install failed.", false); }
    }); })
    .catch(function () { setMsg("stmOtaMsg", "Install request failed.", false); });
}

// ---------- event log ----------
// Codes match event_log.h (MeterEvent).
var EV_MSG = {
  0: "Device restarted",
  1: "R main relay", 2: "Y main relay", 3: "B main relay",
  4: "Y-R aux relay", 5: "B-R aux relay", 6: "R-Y aux relay",
  7: "B-Y aux relay", 8: "R-B aux relay", 9: "Y-B aux relay",
  10: "R phase overload", 11: "Y phase overload", 12: "B phase overload",
  13: "STM32 restarted",
  14: "UPS main relay", 15: "UPS aux relay", 16: "Inverter overload",
  17: "UPS excess power"
};
// ESP8266 reset reason codes (event_log.cpp resetReasonName()).
var RESET_MSG = {
  0: "Power on", 1: "Hardware watchdog", 2: "Exception", 3: "Software watchdog",
  4: "Software restart", 5: "Wake from deep sleep", 6: "External reset"
};

var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
function p2(n) { return (n < 10 ? "0" : "") + n; }
// The device stores a "local epoch" (local wall-clock encoded as UTC), so format
// with UTC getters to avoid the browser shifting it again.
function fmtTs(ts) {
  if (!ts) return "—";
  var d = new Date(ts * 1000);
  var date = p2(d.getUTCDate()) + " " + MONTHS[d.getUTCMonth()] + " " + d.getUTCFullYear();
  var h = d.getUTCHours(), ap = h >= 12 ? "pm" : "am", h12 = (h % 12) || 12;
  var time = h12 + ":" + p2(d.getUTCMinutes()) + ":" + p2(d.getUTCSeconds()) + " " + ap;
  return date + "<br>" + time;
}

function renderLogTable(logs, limit) {
  var n = limit ? Math.min(limit, logs.length) : logs.length;
  var html = '<table style="width:100%; border-collapse:collapse; table-layout:fixed;">';
  for (var i = 0; i < n; i++) {
    var e = logs[i];
    var msg = EV_MSG[e.ev] || ("Event " + e.ev);
    if (e.ev === 0) {
      var reason = RESET_MSG[e.rc] || "Unknown";
      msg += '<br><span style="color:#999;">' + reason + '</span>';
    } else if ((e.ev >= 1 && e.ev <= 9) || e.ev === 14 || e.ev === 15) {
      msg += e.rc ? ' — ON' : ' — OFF';
    } else if ((e.ev >= 10 && e.ev <= 12) || e.ev === 16) {
      msg += e.rc ? ' — overload' : ' — cleared';
    } else if (e.ev === 17) {
      msg += e.rc ? ' — limit exceeded' : ' — cleared';
    }
    html += '<tr style="border-bottom:1px solid #eee;">' +
      '<td style="width:38%; padding:5px 6px; white-space:nowrap; vertical-align:top; color:#555;">' + fmtTs(e.ts) + '</td>' +
      '<td style="padding:5px 6px; vertical-align:top;">' + msg + '</td></tr>';
  }
  html += "</table>";
  return html;
}

var logBusy = false;
function loadHomeLog() {
  var box = $("homeLog");
  if (!box || logBusy) return;
  logBusy = true;
  fetch("/api/getLogs").then(guard)
    .then(function (r) { return r.json(); })
    .then(function (res) {
      var logs = res.logs || [];
      box.innerHTML = logs.length ? renderLogTable(logs, 0) : "No events logged yet.";
    })
    .catch(function () {})
    .then(function () { logBusy = false; });
}

function tabVisible(id) { var el = $(id); return !!el && el.style.display !== "none"; }

// ---------- boot ----------
window.addEventListener("load", function () {
  showContent("menu-1");
  connectLive();     // readings + WiFi/cloud status pushed over the WebSocket
  loadHomeLog();     // log + protection over the HTTP API
  loadProtection();
  // Refresh only the tab being looked at, so the device isn't loaded needlessly.
  setInterval(function () { if (tabVisible("menu-1")) loadHomeLog(); }, 10000);
  setInterval(function () { if (tabVisible("menu-3")) loadProtection(); }, 3000);
});
