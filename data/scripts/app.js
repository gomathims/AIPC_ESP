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

  // Totals (current summed, power summed, energy summed).
  $("iT").textContent = n2(Number(d.i1) + Number(d.i2) + Number(d.i3));
  $("pT").textContent = n1(Number(d.p1) + Number(d.p2) + Number(d.p3));
  $("eT").textContent = n3(Number(d.e1) + Number(d.e2) + Number(d.e3));

  var t = (d.time && d.time.length) ? d.time : "—";
  $("systemTime").textContent = t;
  if ($("rtcTime")) $("rtcTime").textContent = t;
  if ($("battV")) $("battV").textContent = n1(d.batt);
  if ($("batteryChip")) $("batteryChip").classList.toggle("low", Number(d.batt) < 1.5);
}

function pollReadings() {
  fetch("/api/getReadings").then(guard)
    .then(function (r) { return r.json(); })
    .then(applyReadings)
    .catch(function () {});
}

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

function loadWiFiStatus() {
  fetch("/api/getWiFiStatus").then(guard)
    .then(function (r) { return r.json(); })
    .then(function (s) {
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
    })
    .catch(function () {});
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
  postWithPin("/api/setWiFi", { ssid: ssid, pass: $("pass").value }, "wifiMsg", "Saved.",
    function () { setTimeout(loadWiFiStatus, 9000); });
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
    setTimeout(loadWiFiStatus, 4000);
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
function loadCloudStatus() {
  fetch("/api/getCloudStatus").then(guard)
    .then(function (r) { return r.json(); })
    .then(function (c) {
      $("cloudStatusText").textContent = c.message || "—";
      if (c.configured) {
        $("cloudEmailDiv").style.display = "none";
        $("cloudStatusDiv").style.display = "block";
        if (c.email && !$("clEmail").value) $("clEmail").value = c.email;
      } else {
        $("cloudEmailDiv").style.display = "block";
        $("cloudStatusDiv").style.display = "none";
      }
    })
    .catch(function () {});
}

function connectCloud() {
  var email = $("clEmail").value.trim();
  if (email.length < 5 || email.indexOf("@") < 0) { setMsg("cloudMsg", "Enter a valid email.", false); return; }
  postWithPin("/api/connectCloud", { email: email }, "cloudMsg", "Registering…", function () {
    var n = 0; var iv = setInterval(function () { loadCloudStatus(); if (++n >= 8) clearInterval(iv); }, 2500);
  });
}

function disconnectCloud() {
  if (!confirm("Disconnect this device from the EVIQtech cloud?")) return;
  postWithPin("/api/pauseCloud", { operation: "disconnect" }, "cloudMsg", "Disconnected.", loadCloudStatus);
}

// ---------- protection ----------
var protLoaded = false;
function fillProt(i, p) {
  $("protEn" + i).checked = !!p.en;
  $("protHv" + i).value = p.hv;
  $("protLv" + i).value = p.lv;
  $("protHi" + i).value = p.hi;
}
function updProtStatus(i, p) {
  $("protLiveV" + i).textContent = Number(p.v).toFixed(1);
  $("protLiveI" + i).textContent = Number(p.i).toFixed(2);
  var s = $("protStat" + i);
  s.textContent = p.status;
  s.style.color = (p.status === "OK") ? "#1b7a4b"
                : (p.status === "off" || p.status === "—") ? "#999" : "#d32f2f";
}
function loadProtection() {
  fetch("/api/getProtection").then(guard)
    .then(function (r) { return r.json(); })
    .then(function (res) {
      var ph = res.phases || [];
      for (var k = 0; k < ph.length; k++) {
        var i = k + 1;
        if (!protLoaded) fillProt(i, ph[k]);   // don't clobber edits after first load
        updProtStatus(i, ph[k]);
      }
      protLoaded = true;
    })
    .catch(function () {});
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
  for (var i = 1; i <= 3; i++) {
    phases.push({
      en: $("protEn" + i).checked,
      hv: parseFloat($("protHv" + i).value),
      lv: parseFloat($("protLv" + i).value),
      hi: parseFloat($("protHi" + i).value)
    });
  }
  // Validate only the phases the user enabled. Any failure warns via toast (5 s) and stops.
  for (var j = 0; j < 3; j++) {
    var p = phases[j], L = "L" + (j + 1);
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
  13: "STM32 restarted"
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
    } else if (e.ev >= 1 && e.ev <= 9) {
      msg += e.rc ? ' — ON' : ' — OFF';
    } else if (e.ev >= 10 && e.ev <= 12) {
      msg += e.rc ? ' — overload' : ' — cleared';
    }
    html += '<tr style="border-bottom:1px solid #eee;">' +
      '<td style="width:38%; padding:5px 6px; white-space:nowrap; vertical-align:top; color:#555;">' + fmtTs(e.ts) + '</td>' +
      '<td style="padding:5px 6px; vertical-align:top;">' + msg + '</td></tr>';
  }
  html += "</table>";
  return html;
}

function loadHomeLog() {
  var box = $("homeLog");
  if (!box) return;
  fetch("/api/getLogs").then(guard)
    .then(function (r) { return r.json(); })
    .then(function (res) {
      var logs = res.logs || [];
      box.innerHTML = logs.length ? renderLogTable(logs, 0) : "No events logged yet.";
    })
    .catch(function () {});
}

// ---------- boot ----------
window.addEventListener("load", function () {
  showContent("menu-1");
  loadWiFiStatus();
  loadCloudStatus();
  loadHomeLog();
  loadProtection();
  pollReadings();
  setInterval(pollReadings, 3000);
  // setInterval(loadWiFiStatus, 15000);
  // setInterval(loadCloudStatus, 15000);
  // setInterval(loadHomeLog, 5000);
  // setInterval(loadProtection, 3000);
});
