// Generado por tools/web_a_h.py a partir de tools/espejo.html: no editar a mano.
#pragma once
#include <pgmspace.h>

static const char ESPEJO_HTML[] PROGMEM = R"ESPEJO(<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Espejo Ballena</title>
<!--
  Espejo de la pantalla de la ballena (WT32-SC01 Plus) por el cable USB.
  Abrir este archivo en Chrome o Edge (doble clic) → Conectar → elegir el puerto (COMx).
  Antes de subir un programa a la pantalla: Desconectar (o cerrar esta pagina), porque usa el puerto.
  Protocolo: ver "ESPEJO DE PANTALLA POR USB" en BallenaMonitor.ino.
-->
<style>
  :root {
    --bg: #101418; --panel: #1a2027; --line: #2a323c; --text: #e6eaee; --muted: #8c97a3;
    --accent: #3aa7e0; --ok: #3cc47c; --bad: #e0573a;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
    font: 14px/1.4 system-ui, "Segoe UI", sans-serif; }
  body { display: flex; flex-direction: column; }
  header { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 12px; padding: 10px 16px;
    background: var(--panel); border-bottom: 1px solid var(--line); }
  h1 { font-size: 15px; font-weight: 600; margin: 0 8px 0 0; }
  button { font: inherit; color: var(--text); background: #26303a; border: 1px solid var(--line);
    border-radius: 6px; padding: 6px 12px; cursor: pointer; }
  button:hover { border-color: var(--accent); }
  button.primary { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }
  button:disabled { opacity: .45; cursor: default; }
  label { color: var(--muted); display: flex; align-items: center; gap: 4px; cursor: pointer; }
  #estado { display: flex; align-items: center; gap: 6px; color: var(--muted); }
  #punto { width: 9px; height: 9px; border-radius: 50%; background: var(--muted); }
  #punto.ok { background: var(--ok); } #punto.bad { background: var(--bad); }
  #stats { margin-left: auto; color: var(--muted); font-variant-numeric: tabular-nums; }
  main { flex: 1; display: flex; min-height: 0; }
  #zona { flex: 1; display: flex; align-items: center; justify-content: center; padding: 16px; min-width: 0; }
  #zona { overflow: hidden; }
  canvas { background: #000; display: block; flex: none; border-radius: 2px; box-shadow: 0 0 0 1px var(--line); }
  canvas.nitido { image-rendering: pixelated; }
  canvas.tactil { cursor: pointer; touch-action: none; }
  #log { width: 360px; display: none; flex-direction: column; border-left: 1px solid var(--line); background: var(--panel); }
  #log.visible { display: flex; }
  #log pre { flex: 1; margin: 0; padding: 10px; overflow: auto; font: 12px/1.35 ui-monospace, Consolas, monospace;
    color: #c8d0d8; white-space: pre-wrap; word-break: break-all; }
  #frmCmd { margin: 0; border-top: 1px solid var(--line); }
  #cmd { width: 100%; border: 0; padding: 10px; background: #12171c; color: var(--text); font: 12px ui-monospace, Consolas, monospace; outline: none; }
  #aviso { margin: auto; max-width: 520px; padding: 24px; text-align: center; color: var(--muted); }
  #aviso b { color: var(--text); }
  @media (max-width: 800px) { #log { position: fixed; inset: auto 0 0 0; width: auto; height: 40%; } }
</style>
</head>
<body>
<header>
  <h1>Espejo · Ballena</h1>
  <button id="btnCon" class="primary">Conectar</button>
  <span id="estado"><span id="punto"></span><span id="txtEstado">Sin conectar</span></span>
  <label><input type="checkbox" id="chkNitido" checked> Nítido</label>
  <label title="Hacer clic en la imagen = tocar la pantalla de la ballena"><input type="checkbox" id="chkTactil" checked> Clic = tocar</label>
  <label title="Tamaño x2, x3... exacto: cada pixel de la ballena = un cuadro igual"><input type="checkbox" id="chkEntero"> Escala exacta</label>
  <label><input type="checkbox" id="chkLog"> Serial</label>
  <button id="btnFoto" title="Guardar la imagen como PNG">Captura</button>
  <button id="btnFull" title="Pantalla completa">⛶</button>
  <span id="stats"></span>
</header>
<main>
  <div id="zona"><canvas id="cv" width="480" height="320" class="nitido"></canvas></div>
  <aside id="log"><pre id="logTxt"></pre>
    <form id="frmCmd"><input id="cmd" placeholder="Comando (? = estado) y Enter" autocomplete="off"></form></aside>
</main>

<script>
"use strict";
const W = 480, H = 320;
const cv = document.getElementById("cv");
const ctx = cv.getContext("2d");
const img = ctx.createImageData(W, H);
const px32 = new Uint32Array(img.data.buffer);
px32.fill(0xFF000000);
ctx.putImageData(img, 0, 0);

const $ = (id) => document.getElementById(id);
const btnCon = $("btnCon"), punto = $("punto"), txtEstado = $("txtEstado"), stats = $("stats");
const logBox = $("log"), logTxt = $("logTxt");

// Abierta desde la ballena (http://192.168.4.1) = Wi-Fi; abierta como archivo = cable USB (Web Serial)
const MODO_WIFI = location.protocol === "http:" || location.protocol === "https:";
if (!MODO_WIFI && !("serial" in navigator)) {
  $("zona").innerHTML = '<div id="aviso"><b>Este navegador no tiene Web Serial.</b><br>' +
    "Abre este archivo en <b>Google Chrome</b> o <b>Microsoft Edge</b> (en la computadora, no en el celular).</div>";
  btnCon.disabled = true;
}

// RGB565 (little-endian) → RGBA de 32 bits, tabla de 65536 colores
const lut = new Uint32Array(65536);
for (let v = 0; v < 65536; v++) {
  const r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  lut[v] = 0xFF000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
}

let port = null, reader = null, writer = null, keepTimer = null, ws = null;
const conectado = () => MODO_WIFI ? !!(ws && ws.readyState === 1) : !!port;
let quiereConectado = false;
let buf = new Uint8Array(1 << 20), len = 0;
let dx1 = W, dy1 = H, dx2 = -1, dy2 = -1, pintar = false;
let bytesRx = 0, paquetes = 0, malos = 0, ultimoPaquete = 0;
const dec = new TextDecoder();
let lineaTxt = "";

function estado(texto, clase) { txtEstado.textContent = texto; punto.className = clase || ""; }

let tirar = false;   // despues de un paquete roto: tirar los bytes hasta el siguiente "LVM1"
function texto(a, b) {
  if (b <= a) return;
  if (tirar) return;
  lineaTxt += dec.decode(buf.subarray(a, b), { stream: true });
  const partes = lineaTxt.split("\n");
  lineaTxt = partes.pop();
  if (lineaTxt.length > 400) { partes.push(lineaTxt); lineaTxt = ""; }
  for (const l of partes) {
    if (l.includes("ESP-ROM")) { puedeEnviar = false; vioArranque = true; }                          // la pantalla se esta reiniciando
    if (l.includes("[ESPEJO] listo") || l.includes("UI Setup Completed")) puedeEnviar = true;
  }
  if (partes.length) {
    logTxt.textContent += partes.map((l) => l.replace(/[\x00-\x08\x0b-\x1f\x7f�]/g, "")).join("\n") + "\n";
    if (logTxt.textContent.length > 60000) logTxt.textContent = logTxt.textContent.slice(-40000);
    if (logBox.classList.contains("visible")) logTxt.scrollTop = logTxt.scrollHeight;
  }
}

function buscarMagia(i) {
  for (let k = i; k + 3 < len; k++) {
    if (buf[k] === 0x4C && buf[k + 1] === 0x56 && buf[k + 2] === 0x4D && buf[k + 3] === 0x31) return k;   // "LVM1"
  }
  return -1;
}

function procesar() {
  let i = 0;
  for (;;) {
    const m = buscarMagia(i);
    if (m < 0) { const hasta = Math.max(i, len - 3); texto(i, hasta); i = hasta; break; }
    texto(i, m);
    tirar = false;
    if (len - m < 12) { i = m; break; }
    const x = buf[m + 4] | (buf[m + 5] << 8), y = buf[m + 6] | (buf[m + 7] << 8);
    const w = buf[m + 8] | (buf[m + 9] << 8), h = buf[m + 10] | (buf[m + 11] << 8);
    if (w === 0 || h === 0 || x + w > W || y + h > H) { malos++; tirar = true; i = m + 4; continue; }
    const n = 12 + w * h * 2 + 2;
    if (len - m < n) { i = m; break; }
    let suma = 0, k = m + 12;
    for (let j = 0; j < w * h; j++, k += 2) suma = (suma + (buf[k] | (buf[k + 1] << 8))) & 0xFFFF;
    if (suma !== (buf[k] | (buf[k + 1] << 8))) { malos++; tirar = true; i = m + 4; continue; }   // se mezclo con texto: buscar otra vez
    k = m + 12;
    for (let r = 0; r < h; r++) {
      let o = (y + r) * W + x;
      for (let c = 0; c < w; c++, k += 2) px32[o++] = lut[buf[k] | (buf[k + 1] << 8)];
    }
    if (x < dx1) dx1 = x; if (y < dy1) dy1 = y;
    if (x + w - 1 > dx2) dx2 = x + w - 1; if (y + h - 1 > dy2) dy2 = y + h - 1;
    paquetes++; ultimoPaquete = performance.now();
    i = m + n;
  }
  if (i > 0) { buf.copyWithin(0, i, len); len -= i; }
  if (dx2 >= 0 && !pintar) { pintar = true; requestAnimationFrame(dibujar); }
}

function dibujar() {
  pintar = false;
  if (dx2 < 0) return;
  ctx.putImageData(img, 0, 0, dx1, dy1, dx2 - dx1 + 1, dy2 - dy1 + 1);
  dx1 = W; dy1 = H; dx2 = -1; dy2 = -1;
}

function agregar(chunk) {
  bytesRx += chunk.length;
  if (len + chunk.length > buf.length) {
    const nuevo = new Uint8Array(Math.max(buf.length * 2, len + chunk.length));
    nuevo.set(buf.subarray(0, len)); buf = nuevo;
  }
  buf.set(chunk, len); len += chunk.length;
  procesar();
}

// Si la pantalla se reinicia, el canal de escritura puede quedar roto: se vuelve a pedir.
let enviados = 0, erroresTx = 0;
// No mandar nada mientras la pantalla arranca: si llega un byte antes de que el programa
// abra el Serial, la entrada USB del ESP32-S3 se queda trabada hasta desconectar el cable.
let puedeEnviar = false, vioArranque = false;
async function enviar(s) {
  if (MODO_WIFI) {
    if (ws && ws.readyState === 1) { ws.send(s); enviados += s.length; }
    return;
  }
  if (!port || !puedeEnviar) return;
  try {
    if (!writer && port.writable) writer = port.writable.getWriter();
    if (!writer) return;
    await writer.write(new TextEncoder().encode(s));
    enviados += s.length;
  } catch (e) {
    erroresTx++;
    try { writer.releaseLock(); } catch (e2) {}
    writer = null;
  }
}

async function abrir(p) {
  port = p;
  estado("Abriendo…");
  await port.open({ baudRate: 115200, bufferSize: 1 << 16 });
  // DTR=0 y RTS=0 juntos (igual que el Serial Monitor de ESP32). Con DTR=1 al soltar RTS,
  // el ESP32-S3 entra en modo de descarga (pantalla congelada, sin imagen).
  try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch (e) {}
  writer = port.writable.getWriter();
  quiereConectado = true;
  btnCon.textContent = "Desconectar"; btnCon.classList.remove("primary");
  estado("Conectado, esperando imagen…", "ok");
  len = 0; lineaTxt = ""; conectadoEn = performance.now(); vioArranque = false;
  leyendo = leer(port);
  // Al abrir el puerto, Windows reinicia la pantalla: se manda "MF" cada segundo hasta que llega imagen,
  // y despues solo "M" ("sigo aqui").
  puedeEnviar = false;
  setTimeout(() => { if (!vioArranque) puedeEnviar = true; }, 2500);   // no se reinicio: ya se puede
  keepTimer = setInterval(() => enviar(paquetes > 0 && performance.now() - ultimoPaquete < 3000 ? "M" : "MF"), 1000);
}

let leyendo = null, cerrando = false, conectadoEn = 0;

async function leer(p) {
  try {
    reader = p.readable.getReader();
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) agregar(value);
    }
  } catch (e) {
    // se desconecto el cable o se reinicio la placa
  } finally {
    try { reader.releaseLock(); } catch (e) {}
    reader = null;
  }
  if (!cerrando) cerrar(false);
}

async function cerrar(porUsuario) {
  if (cerrando) return;
  cerrando = true;
  if (keepTimer) { clearInterval(keepTimer); keepTimer = null; }
  if (porUsuario) { quiereConectado = false; await enviar("m"); }
  const p = port;
  try { if (reader) await reader.cancel(); } catch (e) {}
  try { if (leyendo) await leyendo; } catch (e) {}
  leyendo = null;
  try { if (writer) writer.releaseLock(); } catch (e) {}
  writer = null;
  try { if (p) await p.close(); } catch (e) {}
  port = null;
  cerrando = false;
  btnCon.textContent = "Conectar"; btnCon.classList.add("primary");
  estado(quiereConectado ? "Cable desconectado: se reconecta solo" : "Sin conectar", quiereConectado ? "bad" : "");
}

btnCon.onclick = async () => {
  if (port) { await cerrar(true); return; }
  try {
    const p = await navigator.serial.requestPort();
    await abrir(p);
  } catch (e) {
    if (e.name !== "NotFoundError") estado("No se pudo abrir: " + e.message + " (¿Serial Monitor abierto?)", "bad");
    else estado("Sin conectar");
    port = null;
  }
};

// al volver a conectar el cable (o despues de reiniciar la placa) se reconecta solo
if ("serial" in navigator) {
  navigator.serial.addEventListener("connect", async (ev) => {
    if (quiereConectado && !port) { try { await abrir(ev.target); } catch (e) { port = null; } }
  });
}

// Tamaño: lo mas grande que quepa, siempre con la proporcion 3:2 de la pantalla (480 x 320), sin estirar
const zona = $("zona");
function ajustar() {
  const r = zona.getBoundingClientRect();
  const aw = r.width - 32, ah = r.height - 32;          // 16 px de margen
  if (aw <= 0 || ah <= 0) return;
  const dpr = window.devicePixelRatio || 1;
  let s = Math.min(aw / W, ah / H);                      // escala en px de CSS
  if ($("chkEntero").checked && s * dpr >= 1) s = Math.floor(s * dpr) / dpr;   // pixeles fisicos enteros
  else s = Math.floor(W * s * dpr) / (W * dpr);          // ancho entero en pixeles fisicos
  cv.style.width = (W * s) + "px";
  cv.style.height = (H * s) + "px";
}
new ResizeObserver(ajustar).observe(zona);
window.addEventListener("resize", ajustar);
$("chkEntero").onchange = ajustar;

// ---- Clic en la imagen = tocar la pantalla de la ballena ----
// Manda "X" + x (3 cifras) + y (3 cifras) + "1"/"0". Mientras se mantiene presionado, se repite cada 150 ms.
let tocando = false, ultimoToque = null, repetirToque = null, ultimoMovMs = 0;
const pad3 = (n) => String(n).padStart(3, "0");
function posToque(ev) {
  const r = cv.getBoundingClientRect();
  const x = Math.min(W - 1, Math.max(0, Math.floor((ev.clientX - r.left) / r.width * W)));
  const y = Math.min(H - 1, Math.max(0, Math.floor((ev.clientY - r.top) / r.height * H)));
  return [x, y];
}
function mandarToque(x, y, presionado) {
  ultimoToque = [x, y];
  enviar("X" + pad3(x) + pad3(y) + (presionado ? "1" : "0"));
}
function soltar(ev) {
  if (!tocando) return;
  tocando = false;
  clearInterval(repetirToque); repetirToque = null;
  if (ev && ev.type === "pointerup") {        // soltar en el punto final (no en el ultimo enviado)
    const [x, y] = posToque(ev);
    mandarToque(x, y, true);
    mandarToque(x, y, false);
  } else if (ultimoToque) mandarToque(ultimoToque[0], ultimoToque[1], false);
}
function tactilOn() { return $("chkTactil").checked && conectado() && paquetes > 0; }
cv.classList.toggle("tactil", $("chkTactil").checked);
$("chkTactil").onchange = (e) => { cv.classList.toggle("tactil", e.target.checked); soltar(null); };
cv.addEventListener("pointerdown", (ev) => {
  if (!tactilOn() || ev.button !== 0) return;
  ev.preventDefault();
  cv.setPointerCapture(ev.pointerId);
  tocando = true;
  const [x, y] = posToque(ev);
  mandarToque(x, y, true);
  repetirToque = setInterval(() => { if (tocando && ultimoToque) mandarToque(ultimoToque[0], ultimoToque[1], true); }, 150);
});
cv.addEventListener("pointermove", (ev) => {
  if (!tocando) return;
  const ahora = performance.now();
  if (ahora - ultimoMovMs < 30) return;   // como maximo ~30 por segundo (arrastrar sliders y el panel EMG)
  ultimoMovMs = ahora;
  const [x, y] = posToque(ev);
  mandarToque(x, y, true);
});
cv.addEventListener("pointerup", soltar);
cv.addEventListener("pointercancel", soltar);
cv.addEventListener("lostpointercapture", soltar);
window.addEventListener("blur", () => soltar(null));
$("frmCmd").onsubmit = (e) => { e.preventDefault(); const c = $("cmd").value; if (c) { logTxt.textContent += "> " + c + "\n"; enviar(c + "\n"); } $("cmd").value = ""; };
$("chkNitido").onchange = (e) => cv.classList.toggle("nitido", e.target.checked);
$("chkLog").onchange = (e) => { logBox.classList.toggle("visible", e.target.checked); logTxt.scrollTop = logTxt.scrollHeight; ajustar(); };
$("btnFull").onclick = () => { if (document.fullscreenElement) document.exitFullscreen(); else zona.requestFullscreen(); };
$("btnFoto").onclick = () => {
  const a = document.createElement("a");
  const t = new Date(); const z = (n) => String(n).padStart(2, "0");
  a.download = `ballena_${t.getFullYear()}${z(t.getMonth() + 1)}${z(t.getDate())}_${z(t.getHours())}${z(t.getMinutes())}${z(t.getSeconds())}.png`;
  a.href = cv.toDataURL("image/png"); a.click();
};

let bytesAntes = 0;
setInterval(() => {
  const kbs = (bytesRx - bytesAntes) / 1024; bytesAntes = bytesRx;
  stats.textContent = conectado() ? `${kbs.toFixed(0)} KB/s · ${paquetes} paquetes${malos ? " · " + malos + " con error" : ""} · enviados ${enviados}${erroresTx ? " (" + erroresTx + " fallaron)" : ""}` : "";
  if (MODO_WIFI) {
    if (conectado()) estado(paquetes > 0 && performance.now() - ultimoPaquete < 4000 ? "Conectado por Wi-Fi" : "Conectado por Wi-Fi, esperando imagen…", "ok");
    return;
  }
  if (port && paquetes > 0 && performance.now() - ultimoPaquete < 4000) estado("Conectado", "ok");
  else if (port && performance.now() - ultimoPaquete > 4000 && bytesRx > 0 && paquetes === 0)
    estado(performance.now() - conectadoEn < 30000 ? "Conectado, esperando a que la pantalla termine de arrancar…"
                                                    : "Conectado, pero sin imagen: ¿la pantalla tiene el programa nuevo?",
           performance.now() - conectadoEn < 30000 ? "ok" : "bad");
}, 1000);

// ---- Wi-Fi: la pagina la sirve la ballena; la imagen llega por WebSocket (puerto 81) ----
function conectarWifi() {
  estado("Conectando por Wi-Fi…");
  try { ws = new WebSocket("ws://" + location.hostname + ":81/"); } catch (e) { setTimeout(conectarWifi, 2000); return; }
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    conectadoEn = performance.now();
    len = 0;
    ws.send("MF");                                   // encender el espejo + mandar todo
    clearInterval(keepTimer);
    keepTimer = setInterval(() => enviar(paquetes > 0 && performance.now() - ultimoPaquete < 3000 ? "M" : "MF"), 1000);
  };
  ws.onmessage = (ev) => { if (ev.data instanceof ArrayBuffer) agregar(new Uint8Array(ev.data)); };
  ws.onclose = () => {
    clearInterval(keepTimer); keepTimer = null;
    soltar(null);
    estado("Sin conexión con la ballena: reintentando… (¿sigue conectada la red " + "PERLA-Ballena?)", "bad");
    setTimeout(conectarWifi, 1500);                  // p. ej. durante ENVIAR se corta unos segundos
  };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}
if (MODO_WIFI) {
  btnCon.style.display = "none";
  $("chkLog").parentElement.style.display = "none";   // los mensajes del Serial solo van por cable
  conectarWifi();
}
</script>
</body>
</html>
)ESPEJO";
