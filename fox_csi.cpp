#include "fox_csi.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_chip_info.h>
#include <Preferences.h>
#ifndef WEBSOCKETS_SERVER_CLIENT_MAX
#define WEBSOCKETS_SERVER_CLIENT_MAX 1
#endif
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Update.h>
#include <math.h>
#include <string.h>

namespace {

Preferences prefs;
bool started = false;

#define CSI_BUF_LEN     64
#define BASELINE_FRAMES 30
#define WF_COLS         32
#define MESH_MAX_NODES  4

uint8_t sensitivity = 5;
uint8_t mode = 0;
bool csiActive = false;

float baseline[CSI_BUF_LEN];
int frameCount = 0;
bool baselineDone = false;

float ampLongEma = 0.0f;
float ampMedEma = 0.0f;
float ampShortEma = 0.0f;
bool vitalsEmaInit = false;
bool breathPrevPos = false;
bool heartPrevPos = false;
int breathCrossCount = 0;
int heartCrossCount = 0;
int vitalsFrameCount = 0;
#define VITALS_WINDOW_FRAMES 300

volatile bool motionPending = false;
volatile uint8_t motionIntensity = 0;
volatile uint8_t motionProximity = 0;

volatile bool waterfallPending = false;
uint8_t waterfallBins[WF_COLS];

volatile bool vitalsPending = false;
volatile uint8_t vitalsBreath = 0;
volatile uint8_t vitalsHeart = 0;

volatile bool channelResultPending = false;
volatile uint8_t channelResult = 0;

enum MeshRole { MeshRolePrimary = 0, MeshRoleSecondary = 1 };
MeshRole meshRole = MeshRolePrimary;

struct MeshNodeReading {
  bool active;
  uint8_t node_id;
  uint8_t intensity;
  uint8_t proximity;
  uint32_t last_seen_ms;
};

struct MeshNodePos {
  int16_t x_cm;
  int16_t y_cm;
};

MeshNodeReading meshReadings[MESH_MAX_NODES];
uint8_t knownMacs[MESH_MAX_NODES][6];
MeshNodePos nodePositions[MESH_MAX_NODES] = {
  {0, 0}, {200, 0}, {200, 200}, {0, 200}
};
float pathlossGamma = 2.0f;

uint8_t meshMyId = 0;
volatile bool meshPaired = false;
uint32_t meshLastHelloMs = 0;
uint32_t meshLastReportMs = 0;
uint32_t meshLastStaleSweepMs = 0;

volatile bool nodeFoundPending = false;
volatile uint8_t nodeFoundId = 0;

volatile bool meshEventPending = false;

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
#define MESH_MAGIC 0xC5
enum MeshPktType { MeshPktHello = 0, MeshPktHelloAck = 1, MeshPktReading = 2 };

struct __attribute__((packed)) MeshPacket {
  uint8_t magic;
  uint8_t type;
  uint8_t node_id;
  uint8_t intensity;
  uint8_t proximity;
};

#define FOX_CSI_AP_SSID  "FoxCSI"
#define FOX_CSI_AP_PASS  "foxcsi123"
#define FOX_CSI_AP_MAX_CONN 4

WebServer* csiHttpServer = nullptr;
WebSocketsServer* csiWsServer = nullptr;
bool webUiActive = false;

void onCsiWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  (void)num; (void)payload; (void)length; (void)type;
}

void wsBroadcast(String json) {
  if (webUiActive && csiWsServer) csiWsServer->broadcastTXT(json);
}

bool jsonExtractLong(const String& json, const char* key, long* out) {
  String needle = String("\"") + key + "\":";
  int idx = json.indexOf(needle);
  if (idx < 0) return false;
  idx += needle.length();
  int end = idx;
  while (end < (int)json.length() &&
         (isDigit(json[end]) || json[end] == '-')) end++;
  if (end == idx) return false;
  *out = json.substring(idx, end).toInt();
  return true;
}

bool jsonExtractString(const String& json, const char* key, String* out) {
  String needle = String("\"") + key + "\":\"";
  int idx = json.indexOf(needle);
  if (idx < 0) return false;
  idx += needle.length();
  int end = json.indexOf('"', idx);
  if (end < 0) return false;
  *out = json.substring(idx, end);
  return true;
}

String bytesToHex(const uint8_t* data, size_t len) {
  static const char* hexchars = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hexchars[(data[i] >> 4) & 0xF];
    out += hexchars[data[i] & 0xF];
  }
  return out;
}

void saveMacTable() {
  prefs.putBytes("mactbl", knownMacs, sizeof(knownMacs));
}
void loadMacTable() {
  memset(knownMacs, 0, sizeof(knownMacs));
  if (prefs.isKey("mactbl")) {
    prefs.getBytes("mactbl", knownMacs, sizeof(knownMacs));
  }
}
bool macIsZero(const uint8_t* mac) {
  for (int i = 0; i < 6; i++) if (mac[i] != 0) return false;
  return true;
}
bool macEq(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}
uint8_t assignOrLookupNodeId(const uint8_t* mac, bool* isNew) {
  *isNew = false;
  for (int id = 1; id < MESH_MAX_NODES; id++) {
    if (macEq(knownMacs[id], mac)) return id;
  }
  for (int id = 1; id < MESH_MAX_NODES; id++) {
    if (macIsZero(knownMacs[id])) {
      memcpy(knownMacs[id], mac, 6);
      saveMacTable();
      *isNew = true;
      return id;
    }
  }
  return 0;
}

float csiAmplitude(int8_t real, int8_t imag) {
  return sqrtf((float)(real * real) + (float)(imag * imag));
}
float sensitivityToThreshold(uint8_t sens) {
  return 8.0f - ((float)sens * 0.7f);
}
uint8_t floatToByte(float val, float maxVal) {
  if (val < 0.0f) val = 0.0f;
  if (val > maxVal) val = maxVal;
  return (uint8_t)((val / maxVal) * 255.0f);
}
uint8_t deltaToProximity(float avgDelta) {
  if (avgDelta < 0.0f) avgDelta = 0.0f;
  if (avgDelta > 15.0f) avgDelta = 15.0f;
  return (uint8_t)((avgDelta / 15.0f) * 100.0f);
}

void csiResetBaseline() {
  baselineDone = false;
  frameCount = 0;
  memset(baseline, 0, sizeof(baseline));
}

void csiSetMode(uint8_t m) {
  if (m > 3) m = 0;
  mode = m;
  csiResetBaseline();
  vitalsEmaInit = false;
  breathCrossCount = 0;
  heartCrossCount = 0;
  vitalsFrameCount = 0;
  breathPrevPos = false;
  heartPrevPos = false;
}

void csiSetSensitivity(uint8_t level) {
  if (level > 10) level = 10;
  sensitivity = level;
}

void vitalsUpdate(float* amp, int nSub) {
  float ampAvg = 0.0f;
  for (int i = 0; i < nSub; i++) ampAvg += amp[i];
  ampAvg /= (float)nSub;

  if (!vitalsEmaInit) {
    ampLongEma = ampAvg;
    ampMedEma = ampAvg;
    ampShortEma = ampAvg;
    vitalsEmaInit = true;
  }

  ampLongEma = 0.99f * ampLongEma + 0.01f * ampAvg;
  ampMedEma = 0.95f * ampMedEma + 0.05f * ampAvg;
  ampShortEma = 0.70f * ampShortEma + 0.30f * ampAvg;

  float breathSignal = ampMedEma - ampLongEma;
  float heartSignal = ampShortEma - ampMedEma;

  bool breathPos = (breathSignal >= 0.0f);
  if (breathPos && !breathPrevPos) breathCrossCount++;
  breathPrevPos = breathPos;

  bool heartPos = (heartSignal >= 0.0f);
  if (heartPos && !heartPrevPos) heartCrossCount++;
  heartPrevPos = heartPos;

  if (++vitalsFrameCount < VITALS_WINDOW_FRAMES) return;

  uint8_t breathingBpm = (uint8_t)((breathCrossCount * 60) / 30);
  uint8_t heartBpm = (uint8_t)((heartCrossCount * 60) / 30);
  if (breathingBpm < 6 || breathingBpm > 40) breathingBpm = 0;
  if (heartBpm < 40 || heartBpm > 180) heartBpm = 0;

  vitalsBreath = breathingBpm;
  vitalsHeart = heartBpm;
  vitalsPending = true;

  breathCrossCount = 0;
  heartCrossCount = 0;
  vitalsFrameCount = 0;
}

void IRAM_ATTR wifiCsiCb(void* ctx, wifi_csi_info_t* info) {
  (void)ctx;
  if (!info || !info->buf) return;

  int8_t* raw = info->buf;
  int nSub = info->len / 2;
  if (nSub > CSI_BUF_LEN) nSub = CSI_BUF_LEN;
  if (nSub <= 0) return;

  float amp[CSI_BUF_LEN];
  memset(amp, 0, sizeof(amp));
  for (int i = 0; i < nSub; i++) {
    amp[i] = csiAmplitude(raw[i * 2], raw[i * 2 + 1]);
  }

  if (!baselineDone) {
    for (int i = 0; i < nSub; i++) baseline[i] += amp[i];
    frameCount++;
    if (frameCount >= BASELINE_FRAMES) {
      for (int i = 0; i < nSub; i++) baseline[i] /= (float)BASELINE_FRAMES;
      baselineDone = true;
    }
    return;
  }

  float wfBins[CSI_BUF_LEN];
  float totalDelta = 0.0f;
  for (int i = 0; i < nSub; i++) {
    float d = fabsf(amp[i] - baseline[i]);
    wfBins[i] = d;
    totalDelta += d;
  }
  float avgDelta = totalDelta / (float)nSub;

  const float alpha = 0.02f;
  for (int i = 0; i < nSub; i++) {
    baseline[i] = baseline[i] * (1.0f - alpha) + amp[i] * alpha;
  }

  float threshold = sensitivityToThreshold(sensitivity);

  switch (mode) {
    case 0:
    case 2: {
      if (avgDelta > threshold) {
        uint8_t intensity = floatToByte(avgDelta, 20.0f);
        uint8_t proximity = deltaToProximity(avgDelta);
        if (meshRole == MeshRolePrimary) {
          motionIntensity = intensity;
          motionProximity = proximity;
          motionPending = true;
          meshReadings[0].active = true;
          meshReadings[0].node_id = 0;
          meshReadings[0].intensity = intensity;
          meshReadings[0].proximity = proximity;
          meshReadings[0].last_seen_ms = millis();
        } else if (meshPaired) {
          MeshPacket pkt = {MESH_MAGIC, MeshPktReading, meshMyId, intensity, proximity};
          esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, sizeof(pkt));
        }
      }
      break;
    }
    case 1: {
      uint8_t out[WF_COLS];
      memset(out, 0, sizeof(out));
      int binsPerCol = nSub / WF_COLS;
      if (binsPerCol < 1) binsPerCol = 1;
      for (int c = 0; c < WF_COLS; c++) {
        float sum = 0.0f;
        for (int b = 0; b < binsPerCol; b++) {
          int idx = c * binsPerCol + b;
          if (idx < nSub) sum += wfBins[idx];
        }
        out[c] = floatToByte(sum / (float)binsPerCol, 20.0f);
      }
      memcpy(waterfallBins, out, WF_COLS);
      waterfallPending = true;
      break;
    }
    case 3:
      vitalsUpdate(amp, nSub);
      break;
    default:
      break;
  }
}

volatile uint16_t surveyCount = 0;

void IRAM_ATTR surveyPromiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  (void)buf; (void)type;
  surveyCount++;
}

uint8_t runChannelSurvey() {
  bool wasActive = csiActive;
  if (wasActive) esp_wifi_set_csi(false);

  esp_wifi_set_promiscuous_rx_cb(&surveyPromiscCb);
  esp_wifi_set_promiscuous(true);

  uint8_t bestCh = 6;
  uint16_t bestCnt = 0;
  for (uint8_t ch = 1; ch <= 13; ch++) {
    surveyCount = 0;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    delay(1000);
    uint16_t cnt = surveyCount;
    if (cnt > bestCnt) { bestCnt = cnt; bestCh = ch; }
  }

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(bestCh, WIFI_SECOND_CHAN_NONE);
  csiResetBaseline();
  if (wasActive) esp_wifi_set_csi(true);

  return bestCh;
}

#define PROX_MAX_RANGE_CM 500.0f
#define TRILAT_ITERATIONS 6
#define TRILAT_STEP 0.35f

float proximityToDistanceCm(uint8_t proximity) {
  float p = proximity / 100.0f;
  if (p > 1.0f) p = 1.0f;
  if (p < 0.0f) p = 0.0f;
  float frac = 1.0f - p;
  return PROX_MAX_RANGE_CM * powf(frac, pathlossGamma);
}

uint8_t meshGetReadings(MeshNodeReading* out, uint8_t maxCount) {
  uint8_t n = 0;
  for (int i = 0; i < MESH_MAX_NODES && n < maxCount; i++) {
    if (meshReadings[i].active) out[n++] = meshReadings[i];
  }
  return n;
}

bool meshEstimatePosition(int16_t* outXCm, int16_t* outYCm) {
  MeshNodeReading readings[MESH_MAX_NODES];
  uint8_t n = meshGetReadings(readings, MESH_MAX_NODES);
  if (n < 2) return false;

  float distCm[MESH_MAX_NODES] = {0};
  bool haveDist[MESH_MAX_NODES] = {false};

  float wx = 0, wy = 0, wsum = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t id = readings[i].node_id;
    float w = (float)readings[i].intensity + 1.0f;
    wx += (float)nodePositions[id].x_cm * w;
    wy += (float)nodePositions[id].y_cm * w;
    wsum += w;
    distCm[id] = proximityToDistanceCm(readings[i].proximity);
    haveDist[id] = true;
  }
  if (wsum <= 0.0f) return false;

  float ex = wx / wsum;
  float ey = wy / wsum;

  for (int iter = 0; iter < TRILAT_ITERATIONS; iter++) {
    for (uint8_t i = 0; i < n; i++) {
      uint8_t id = readings[i].node_id;
      if (!haveDist[id]) continue;
      float dx = ex - (float)nodePositions[id].x_cm;
      float dy = ey - (float)nodePositions[id].y_cm;
      float curD = sqrtf(dx * dx + dy * dy);
      if (curD < 1.0f) curD = 1.0f;
      float error = distCm[id] - curD;
      float ux = dx / curD;
      float uy = dy / curD;
      ex += ux * error * TRILAT_STEP;
      ey += uy * error * TRILAT_STEP;
    }
  }

  *outXCm = (int16_t)ex;
  *outYCm = (int16_t)ey;
  return true;
}

void meshRecvCbPrimary(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(MeshPacket)) return;
  const MeshPacket* pkt = (const MeshPacket*)data;
  if (pkt->magic != MESH_MAGIC) return;

  if (pkt->type == MeshPktHello) {
    bool isNew = false;
    uint8_t id = assignOrLookupNodeId(info->src_addr, &isNew);
    if (id == 0) return;

    MeshPacket ack = {MESH_MAGIC, MeshPktHelloAck, id, 0, 0};
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, info->src_addr, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    if (!esp_now_is_peer_exist(info->src_addr)) esp_now_add_peer(&peer);
    esp_now_send(info->src_addr, (const uint8_t*)&ack, sizeof(ack));

    if (isNew) {
      nodeFoundId = id;
      nodeFoundPending = true;
    }
    return;
  }

  if (pkt->type == MeshPktReading) {
    if (pkt->node_id == 0 || pkt->node_id >= MESH_MAX_NODES) return;
    if (!macEq(knownMacs[pkt->node_id], info->src_addr)) return;
    meshReadings[pkt->node_id].active = true;
    meshReadings[pkt->node_id].node_id = pkt->node_id;
    meshReadings[pkt->node_id].intensity = pkt->intensity;
    meshReadings[pkt->node_id].proximity = pkt->proximity;
    meshReadings[pkt->node_id].last_seen_ms = millis();
  }
}

void meshRecvCbSecondary(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
  if (len != (int)sizeof(MeshPacket)) return;
  const MeshPacket* pkt = (const MeshPacket*)data;
  if (pkt->magic != MESH_MAGIC || pkt->type != MeshPktHelloAck) return;
  if (!meshPaired) {
    meshMyId = pkt->node_id;
    meshPaired = true;
  }
}

void meshBringUpEspNow() {
  esp_now_init();
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(BROADCAST_MAC)) esp_now_add_peer(&peer);
}

void meshInitPrimary() {
  memset(meshReadings, 0, sizeof(meshReadings));
  loadMacTable();
  meshBringUpEspNow();
  esp_now_register_recv_cb(meshRecvCbPrimary);
}

void meshInitSecondary() {
  meshPaired = false;
  meshMyId = 0;
  meshBringUpEspNow();
  esp_now_register_recv_cb(meshRecvCbSecondary);
}

void meshForgetAllNodes() {
  memset(knownMacs, 0, sizeof(knownMacs));
  memset(meshReadings, 0, sizeof(meshReadings));
  saveMacTable();
}

const char CSI_WEBUI_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><title>Fox CSI</title><meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:monospace;background:#111;color:#0f0;margin:0;padding:12px}
h2{margin:0 0 8px 0}
canvas{background:#000;border:1px solid #0f0}
#stats{margin-top:8px;white-space:pre}
#ota{margin-top:16px;border-top:1px solid #333;padding-top:8px}
</style></head><body>
<h2>Fox CSI Radar</h2>
<canvas id='radar' width='240' height='240'></canvas>
<div id='stats'>connecting...</div>
<div id='ota'>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='firmware'>
<input type='submit' value='Update Firmware (OTA)'>
</form>
</div>
<script>
var cv=document.getElementById('radar'),ctx=cv.getContext('2d');
var cx=120,cy=120,r=110;
var nodes={};
function drawRadar(intensity,proximity){
  ctx.clearRect(0,0,240,240);
  ctx.strokeStyle='#0f0';
  ctx.beginPath();ctx.arc(cx,cy,r,0,7);ctx.stroke();
  ctx.beginPath();ctx.arc(cx,cy,r*0.66,0,7);ctx.stroke();
  ctx.beginPath();ctx.arc(cx,cy,r*0.33,0,7);ctx.stroke();
  if(intensity>0){
    var d=r*(1-(proximity/100));
    var a=Math.random()*6.28;
    ctx.fillStyle='#0f0';
    ctx.beginPath();ctx.arc(cx+Math.cos(a)*d,cy+Math.sin(a)*d,4,0,7);ctx.fill();
  }
  for(var id in nodes){
    var n=nodes[id];
    ctx.fillStyle='#0ff';
    ctx.fillRect(cx+n.x/5-2,cy+n.y/5-2,4,4);
  }
}
var stats=document.getElementById('stats');
function connect(){
  var ws=new WebSocket('ws://'+location.hostname+':81/');
  ws.onmessage=function(ev){
    var d=JSON.parse(ev.data);
    if(d.type==='motion'){drawRadar(d.intensity,d.proximity);
      stats.textContent='intensity='+d.intensity+' proximity='+d.proximity;}
    else if(d.type==='vitals'){
      stats.textContent='breathing='+d.breath+' bpm  heart='+d.heart+' bpm';}
    else if(d.type==='mesh'){
      nodes={};
      (d.nodes||[]).forEach(function(n){nodes[n.id]={x:0,y:0,intensity:n.intensity};});
      drawRadar(0,0);}
    else if(d.type==='node_found'){
      stats.textContent='node '+d.id+' discovered';}
  };
  ws.onclose=function(){setTimeout(connect,2000);};
}
connect();
</script>
</body></html>
)HTML";

void handleWebUiRoot() {
  csiHttpServer->send_P(200, "text/html", CSI_WEBUI_HTML);
}

void handleOtaResult() {
  csiHttpServer->send(200, "text/plain", Update.hasError() ? "OTA FAILED" : "OTA OK, rebooting");
  delay(500);
  ESP.restart();
}
void handleOtaUpload() {
  HTTPUpload& upload = csiHttpServer->upload();
  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void webUiStart() {
  if (webUiActive) return;

  uint8_t apChannel = WiFi.channel();
  if (apChannel < 1 || apChannel > 13) apChannel = 6;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(FOX_CSI_AP_SSID, FOX_CSI_AP_PASS, apChannel, 0, FOX_CSI_AP_MAX_CONN);

  if (!csiHttpServer) csiHttpServer = new WebServer(80);
  if (!csiWsServer) csiWsServer = new WebSocketsServer(81);

  csiHttpServer->on("/", HTTP_GET, handleWebUiRoot);
  csiHttpServer->on("/update", HTTP_POST, handleOtaResult, handleOtaUpload);
  csiHttpServer->begin();

  csiWsServer->begin();
  csiWsServer->onEvent(onCsiWsEvent);

  webUiActive = true;
}

void webUiStop() {
  if (!webUiActive) return;
  if (csiHttpServer) csiHttpServer->stop();
  if (csiWsServer) csiWsServer->close();
  WiFi.softAPdisconnect(true);
  if (WiFi.getMode() == WIFI_AP_STA) WiFi.mode(WIFI_STA);
  webUiActive = false;
}

}

namespace FoxCsi {

void begin() {
  prefs.begin("foxcsi", false);
  meshRole = (prefs.getUChar("role", 0) == 1) ? MeshRoleSecondary : MeshRolePrimary;
  pathlossGamma = (float)prefs.getUChar("gamma_x10", 20) / 10.0f;
  bool wantWebUi = prefs.getBool("webui", false);

  if (prefs.isKey("positions")) {
    int16_t buf[6];
    prefs.getBytes("positions", buf, sizeof(buf));
    for (int n = 1; n <= 3; n++) {
      nodePositions[n].x_cm = buf[(n - 1) * 2];
      nodePositions[n].y_cm = buf[(n - 1) * 2 + 1];
    }
  }

  if (meshRole == MeshRolePrimary) {
    meshInitPrimary();
  } else {
    meshInitSecondary();
  }

  started = true;

  if (meshRole == MeshRolePrimary && wantWebUi) {
    webUiStart();
  }
}

void loop() {
  if (!started) return;

  if (motionPending) {
    motionPending = false;
    Serial.print("[CSI/EVENT/MOTION]{\"intensity\":");
    Serial.print(motionIntensity);
    Serial.print(",\"proximity\":");
    Serial.print(motionProximity);
    Serial.println("}");
    wsBroadcast("{\"type\":\"motion\",\"intensity\":" + String(motionIntensity) +
                ",\"proximity\":" + String(motionProximity) + "}");
  }

  if (waterfallPending) {
    waterfallPending = false;
    String hex = bytesToHex(waterfallBins, WF_COLS);
    Serial.print("[CSI/EVENT/WATERFALL]{\"bins\":\"");
    Serial.print(hex);
    Serial.println("\"}");
    wsBroadcast("{\"type\":\"waterfall\",\"bins\":\"" + hex + "\"}");
  }

  if (vitalsPending) {
    vitalsPending = false;
    Serial.print("[CSI/EVENT/VITALS]{\"breath\":");
    Serial.print(vitalsBreath);
    Serial.print(",\"heart\":");
    Serial.print(vitalsHeart);
    Serial.println("}");
    wsBroadcast("{\"type\":\"vitals\",\"breath\":" + String(vitalsBreath) +
                ",\"heart\":" + String(vitalsHeart) + "}");
  }

  if (channelResultPending) {
    channelResultPending = false;
    Serial.print("[CSI/CHANNEL/SET]{\"ch\":");
    Serial.print(channelResult);
    Serial.println("}");
  }

  if (meshRole == MeshRolePrimary) {
    if (nodeFoundPending) {
      nodeFoundPending = false;
      Serial.print("[CSI/EVENT/NODE_FOUND]{\"id\":");
      Serial.print(nodeFoundId);
      Serial.println("}");
      wsBroadcast("{\"type\":\"node_found\",\"id\":" + String(nodeFoundId) + "}");
    }

    uint32_t now = millis();
    if (now - meshLastStaleSweepMs > 1000) {
      meshLastStaleSweepMs = now;
      for (int i = 1; i < MESH_MAX_NODES; i++) {
        if (meshReadings[i].active && (now - meshReadings[i].last_seen_ms) > 3000) {
          meshReadings[i].active = false;
        }
      }
    }

    if (now - meshLastReportMs > 500) {
      meshLastReportMs = now;
      MeshNodeReading readings[MESH_MAX_NODES];
      uint8_t n = meshGetReadings(readings, MESH_MAX_NODES);
      if (n > 0) {
        int16_t estX = 0, estY = 0;
        bool hasEst = meshEstimatePosition(&estX, &estY);
        String body = "\"has_est\":";
        body += hasEst ? "1" : "0";
        body += ",\"est_x\":" + String(estX);
        body += ",\"est_y\":" + String(estY);
        body += ",\"nodes\":[";
        for (uint8_t i = 0; i < n; i++) {
          if (i > 0) body += ",";
          body += "{\"id\":" + String(readings[i].node_id);
          body += ",\"intensity\":" + String(readings[i].intensity);
          body += ",\"proximity\":" + String(readings[i].proximity) + "}";
        }
        body += "]";
        Serial.println("[CSI/EVENT/MESH]{" + body + "}");
        wsBroadcast("{\"type\":\"mesh\"," + body + "}");
      }
    }
  } else if (!meshPaired) {
    uint32_t now = millis();
    if (now - meshLastHelloMs > 2000) {
      meshLastHelloMs = now;
      MeshPacket hello = {MESH_MAGIC, MeshPktHello, 0, 0, 0};
      esp_now_send(BROADCAST_MAC, (const uint8_t*)&hello, sizeof(hello));
    }
  }

  if (webUiActive && csiHttpServer && csiWsServer) {
    csiHttpServer->handleClient();
    csiWsServer->loop();
  }
}

bool handleCommand(const String& line) {
  if (!line.startsWith("[CSI/")) return false;
  int closeBracket = line.indexOf(']');
  if (closeBracket < 0) return false;
  String cmd = line.substring(1, closeBracket);
  String rest = line.substring(closeBracket + 1);
  rest.trim();

  if (cmd == "CSI/HELLO") {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char* chipName = "ESP32";
    uint8_t csiSupport = 2;
    switch (chip.model) {
      case CHIP_ESP32: chipName = "ESP32"; csiSupport = 1; break;
      case CHIP_ESP32S2: chipName = "ESP32-S2"; csiSupport = 1; break;
      case CHIP_ESP32S3: chipName = "ESP32-S3"; csiSupport = 2; break;
      case CHIP_ESP32C3: chipName = "ESP32-C3"; csiSupport = 2; break;
      case CHIP_ESP32C6: chipName = "ESP32-C6"; csiSupport = 2; break;
      default: chipName = "ESP32-??"; csiSupport = 1; break;
    }
    Serial.print("[CSI/HELLO/SUCCESS]{\"chip\":\"");
    Serial.print(chipName);
    Serial.print("\",\"csi_support\":");
    Serial.print(csiSupport);
    Serial.print(",\"fw_major\":1,\"fw_minor\":0,\"webui\":");
    Serial.print(webUiActive ? 1 : 0);
    Serial.print(",\"role\":\"");
    Serial.print(meshRole == MeshRolePrimary ? "primary" : "secondary");
    Serial.println("\"}");
    return true;
  }

  if (cmd == "CSI/START") {
    long m = 0;
    if (jsonExtractLong(rest, "mode", &m)) csiSetMode((uint8_t)m);
    if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);

#if CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32C61 || CONFIG_IDF_TARGET_ESP32C5
    wifi_csi_acquire_config_t csiCfg = {};
    csiCfg.enable = 1;
    csiCfg.acquire_csi_legacy = 1;
    csiCfg.acquire_csi_ht20 = 1;
#else
    wifi_csi_config_t csiCfg = {};
    csiCfg.lltf_en = true;
    csiCfg.htltf_en = true;
    csiCfg.stbc_htltf2_en = true;
    csiCfg.ltf_merge_en = true;
    csiCfg.channel_filter_en = false;
    csiCfg.manu_scale = false;
#endif
    esp_wifi_set_csi_config(&csiCfg);
    esp_wifi_set_csi_rx_cb(wifiCsiCb, NULL);
    esp_wifi_set_csi(true);
    csiResetBaseline();
    csiActive = true;
    Serial.println("[CSI/START/SUCCESS]");
    return true;
  }

  if (cmd == "CSI/STOP") {
    esp_wifi_set_csi(false);
    csiActive = false;
    Serial.println("[CSI/STOP/SUCCESS]");
    return true;
  }

  if (cmd == "CSI/MODE") {
    long m = 0;
    if (jsonExtractLong(rest, "mode", &m)) {
      csiSetMode((uint8_t)m);
      Serial.println("[CSI/MODE/SUCCESS]");
    }
    return true;
  }

  if (cmd == "CSI/SENSITIVITY") {
    long lvl = 0;
    if (jsonExtractLong(rest, "level", &lvl)) {
      csiSetSensitivity((uint8_t)lvl);
      Serial.println("[CSI/SENSITIVITY/SUCCESS]");
    }
    return true;
  }

  if (cmd == "CSI/CALIBRATE") {
    csiResetBaseline();
    Serial.println("[CSI/CALIBRATE/SUCCESS]");
    return true;
  }

  if (cmd == "CSI/CHANNEL/AUTO") {
    channelResult = runChannelSurvey();
    channelResultPending = true;
    return true;
  }

  if (cmd == "CSI/CHANNEL") {
    long ch = 0;
    if (jsonExtractLong(rest, "ch", &ch) && ch >= 1 && ch <= 13) {
      esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);
      csiResetBaseline();
      Serial.println("[CSI/CHANNEL/SUCCESS]");
    }
    return true;
  }

  if (cmd == "CSI/MESH/ROLE") {
    String role;
    if (jsonExtractString(rest, "role", &role)) {
      MeshRole newRole = (role == "secondary") ? MeshRoleSecondary : MeshRolePrimary;
      if (newRole != meshRole) {
        meshRole = newRole;
        prefs.putUChar("role", meshRole == MeshRoleSecondary ? 1 : 0);
        if (meshRole == MeshRolePrimary) {
          meshInitPrimary();
        } else {
          webUiStop();
          meshInitSecondary();
        }
      }
      Serial.println("[CSI/MESH/ROLE/SUCCESS]");
    }
    return true;
  }

  if (cmd == "CSI/MESH/FORGET") {
    meshForgetAllNodes();
    Serial.println("[CSI/MESH/FORGET/SUCCESS]");
    return true;
  }

  if (cmd == "CSI/MESH/POSITIONS") {
    long x1, y1, x2, y2, x3, y3;
    if (jsonExtractLong(rest, "x1", &x1) && jsonExtractLong(rest, "y1", &y1) &&
        jsonExtractLong(rest, "x2", &x2) && jsonExtractLong(rest, "y2", &y2) &&
        jsonExtractLong(rest, "x3", &x3) && jsonExtractLong(rest, "y3", &y3)) {
      nodePositions[1] = {(int16_t)x1, (int16_t)y1};
      nodePositions[2] = {(int16_t)x2, (int16_t)y2};
      nodePositions[3] = {(int16_t)x3, (int16_t)y3};
      int16_t buf[6] = {(int16_t)x1, (int16_t)y1, (int16_t)x2,
                         (int16_t)y2, (int16_t)x3, (int16_t)y3};
      prefs.putBytes("positions", buf, sizeof(buf));
      Serial.println("[CSI/MESH/POSITIONS/SUCCESS]");
    }
    return true;
  }

  if (cmd == "CSI/MESH/GAMMA") {
    long gammaX10 = 0;
    if (jsonExtractLong(rest, "gamma", &gammaX10)) {
      if (gammaX10 < 10) gammaX10 = 10;
      if (gammaX10 > 50) gammaX10 = 50;
      pathlossGamma = (float)gammaX10 / 10.0f;
      prefs.putUChar("gamma_x10", (uint8_t)gammaX10);
      Serial.println("[CSI/MESH/GAMMA/SUCCESS]");
    }
    return true;
  }

  if (cmd == "CSI/WEBUI/ON") {
    if (meshRole == MeshRolePrimary) {
      webUiStart();
      prefs.putBool("webui", true);
    }
    Serial.println("[CSI/WEBUI/ON/SUCCESS]");
    return true;
  }

  if (cmd == "CSI/WEBUI/OFF") {
    webUiStop();
    prefs.putBool("webui", false);
    Serial.println("[CSI/WEBUI/OFF/SUCCESS]");
    return true;
  }

  return false;
}

}
