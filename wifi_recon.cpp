#include "wifi_recon.h"
#include "config.h"
#include "gps.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <string.h>

namespace {
struct ApEntry {
  String ssid;
  uint8_t bssid[6];
  int32_t rssi;
  uint8_t channel;
  wifi_auth_mode_t encType;
};

struct StaEntry {
  uint8_t mac[6];
  int32_t rssi;
};

ApEntry apList[WIFI_SCAN_MAX_RESULTS];
int apCount = 0;
int selectedApIndex = -1;

StaEntry staList[WIFI_STA_SCAN_MAX_RESULTS];
int staCount = 0;
int selectedStaIndex = -1;

enum class PromiscMode {
  NONE, STA_SNIFF, SIGMON, PACKETCOUNT,
  SNIFF_BEACON, SNIFF_DEAUTH, SNIFF_PROBE, SNIFF_RAW, SNIFF_MULTISSID, SNIFF_PMKID, SNIFF_SAE,
  CAPTURE, MACTRACK
};
volatile PromiscMode promiscMode = PromiscMode::NONE;
volatile uint32_t promiscCounter = 0;
uint8_t sigmonTargetBssid[6];

String multiSsidSeen[WIFI_SNIFF_MULTISSID_SSID_MAX];
int multiSsidCount = 0;

char macTrackList[WIFI_MACTRACK_MAX_MACS][18];
volatile int macTrackCount = 0;

uint8_t factoryStaMac[6];
bool factoryMacCaptured = false;

String macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool macEqual(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
  return true;
}

bool macFromString(const String& s, uint8_t out[6]) {
  unsigned int values[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)values[i];
  return true;
}

void randomLocalMac(uint8_t out[6]) {
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)random(0, 256);
  out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);
}

void captureFactoryMacIfNeeded() {
  if (!factoryMacCaptured) {
    esp_wifi_get_mac(WIFI_IF_STA, factoryStaMac);
    factoryMacCaptured = true;
  }
}

const char* encTypeLabel(wifi_auth_mode_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "UNKNOWN";
  }
}

void printApEntry(int index) {
  if (index < 0 || index >= apCount) return;
  const ApEntry& e = apList[index];
  Serial.print("AP:"); Serial.print(index);
  Serial.print(" ssid:\""); Serial.print(e.ssid);
  Serial.print("\" bssid:"); Serial.print(macToString(e.bssid));
  Serial.print(" ch:"); Serial.print(e.channel);
  Serial.print(" rssi:"); Serial.print(e.rssi);
  Serial.print(" enc:"); Serial.println(encTypeLabel(e.encType));
}

void printStaEntry(int index) {
  if (index < 0 || index >= staCount) return;
  const StaEntry& e = staList[index];
  Serial.print("STA:"); Serial.print(index);
  Serial.print(" mac:"); Serial.print(macToString(e.mac));
  Serial.print(" rssi:"); Serial.println(e.rssi);
}

void IRAM_ATTR promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  auto* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  int rssi = pkt->rx_ctrl.rssi;

  switch (promiscMode) {
    case PromiscMode::PACKETCOUNT:
      promiscCounter++;
      return;

    case PromiscMode::STA_SNIFF: {
      if (len < 24) return;
      if (payload[0] != 0x40) return;
      const uint8_t* srcMac = payload + 10;
      for (int i = 0; i < staCount; i++) {
        if (macEqual(staList[i].mac, srcMac)) { staList[i].rssi = rssi; return; }
      }
      if (staCount < WIFI_STA_SCAN_MAX_RESULTS) {
        memcpy(staList[staCount].mac, srcMac, 6);
        staList[staCount].rssi = rssi;
        staCount++;
      }
      return;
    }

    case PromiscMode::SIGMON: {
      if (len < 24) return;
      uint8_t frameType = payload[0] & 0xFC;
      if ((frameType & 0x0C) != 0x00) return;
      const uint8_t* bssid = payload + 16;
      if (macEqual(bssid, sigmonTargetBssid)) {
        Serial.print("RSSI:"); Serial.println(rssi);
      }
      return;
    }

    case PromiscMode::SNIFF_BEACON: {
      if (len < 24) return;
      if (payload[0] != 0x80) return;
      const uint8_t* bssid = payload + 16;
      Serial.print("BEACON:"); Serial.print(macToString(bssid));
      Serial.print(" rssi:"); Serial.println(rssi);
      return;
    }

    case PromiscMode::SNIFF_DEAUTH: {
      if (len < 24) return;
      uint8_t fc0 = payload[0];
      if (fc0 != 0xC0 && fc0 != 0xA0) return;
      const uint8_t* src = payload + 10;
      const uint8_t* dst = payload + 4;
      Serial.print(fc0 == 0xC0 ? "DEAUTH:" : "DISASSOC:");
      Serial.print(macToString(src)); Serial.print("->"); Serial.println(macToString(dst));
      return;
    }

    case PromiscMode::SNIFF_PROBE: {
      if (len < 24) return;
      if (payload[0] != 0x40 && payload[0] != 0x50) return;
      const uint8_t* src = payload + 10;
      Serial.print(payload[0] == 0x40 ? "PROBEREQ:" : "PROBERESP:");
      Serial.println(macToString(src));
      return;
    }

    case PromiscMode::SNIFF_RAW: {
      if (len < 24) return;
      Serial.print("RAW:len:"); Serial.print(len);
      Serial.print(" rssi:"); Serial.println(rssi);
      return;
    }

    case PromiscMode::SNIFF_MULTISSID: {
      if (len < 38) return;
      if (payload[0] != 0x80) return;
      int idx = 36;
      if (payload[idx] != 0x00) return;
      uint8_t ssidLen = payload[idx + 1];
      if (ssidLen > 32 || idx + 2 + ssidLen > len) return;
      char ssidBuf[33];
      memcpy(ssidBuf, payload + idx + 2, ssidLen);
      ssidBuf[ssidLen] = 0;
      String ssid(ssidBuf);
      for (int i = 0; i < multiSsidCount; i++) { if (multiSsidSeen[i] == ssid) return; }
      if (multiSsidCount < WIFI_SNIFF_MULTISSID_SSID_MAX) {
        multiSsidSeen[multiSsidCount++] = ssid;
        Serial.print("MULTISSID:"); Serial.print(multiSsidCount);
        Serial.print(" newssid:"); Serial.println(ssid);
        if (multiSsidCount >= WIFI_SNIFF_MULTISSID_THRESHOLD) Serial.println("MULTISSID:FLOODLIKELY");
      }
      return;
    }

    case PromiscMode::SNIFF_PMKID: {
      if (len < 24) return;
      uint8_t fc0 = payload[0];
      if ((fc0 & 0x0C) != 0x08) return;
      int offsets[2] = {24, 26};
      for (int oi = 0; oi < 2; oi++) {
        int hdrLen = offsets[oi];
        if (hdrLen + 8 > len) continue;
        const uint8_t* llc = payload + hdrLen;
        if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03) {
          uint16_t etherType = (uint16_t)((llc[6] << 8) | llc[7]);
          if (etherType == 0x888E) {
            const uint8_t* bssid = payload + 16;
            Serial.print("PMKID:possible_eapol_seen bssid:");
            Serial.println(macToString(bssid));
            return;
          }
        }
      }
      return;
    }

    case PromiscMode::SNIFF_SAE: {
      if (len < 26) return;
      if (payload[0] != 0xB0) return;
      uint16_t authAlg = (uint16_t)(payload[24] | (payload[25] << 8));
      if (authAlg != 3) return;
      const uint8_t* src = payload + 10;
      Serial.print("SAE:commit_from:"); Serial.println(macToString(src));
      return;
    }

    case PromiscMode::CAPTURE: {
      if (len < 4) return;
      if (promiscCounter >= (uint32_t)WIFI_PCAP_MAX_FRAMES) return;
      promiscCounter++;
      int origLen = len;
      int capLen = origLen < WIFI_PCAP_SNAPLEN ? origLen : WIFI_PCAP_SNAPLEN;
      Serial.print("PCAPPKT:"); Serial.print(capLen); Serial.print(":"); Serial.print(origLen);
      Serial.print(":"); Serial.println(millis());
      for (int off = 0; off < capLen; off += WIFI_PCAP_CHUNK_BYTES) {
        int chunkLen = capLen - off;
        if (chunkLen > WIFI_PCAP_CHUNK_BYTES) chunkLen = WIFI_PCAP_CHUNK_BYTES;
        Serial.print("PCAPDATA:");
        for (int i = 0; i < chunkLen; i++) {
          if (payload[off + i] < 0x10) Serial.print('0');
          Serial.print(payload[off + i], HEX);
        }
        Serial.println();
      }
      Serial.println("PCAPEND");
      return;
    }

    case PromiscMode::MACTRACK: {
      if (len < 10) return;
      if ((payload[0] & 0x0C) == 0x04) return;
      const uint8_t* src = payload + 10;
      if (src[0] & 0x01) return;
      int n = macTrackCount;
      for (int i = 0; i < n; i++) {
        const char* entry = macTrackList[i];
        bool match = true;
        for (int b = 0; b < 6 && match; b++) {
          uint8_t hi = (src[b] >> 4) & 0x0F;
          uint8_t lo = src[b] & 0x0F;
          int entryOffset = b * 3;
          char ehi = (hi < 10) ? ('0' + hi) : ('A' + hi - 10);
          char elo = (lo < 10) ? ('0' + lo) : ('A' + lo - 10);
          if (entry[entryOffset] != ehi || entry[entryOffset + 1] != elo) match = false;
        }
        if (match) return;
      }
      if (n >= WIFI_MACTRACK_MAX_MACS) return;
      snprintf(macTrackList[n], 18, "%02X:%02X:%02X:%02X:%02X:%02X",
               src[0], src[1], src[2], src[3], src[4], src[5]);
      macTrackCount = n + 1;
      return;
    }

    default: return;
  }
}

void stopPromiscuous() {
  esp_wifi_set_promiscuous(false);
  promiscMode = PromiscMode::NONE;
}

void startPromiscuous(PromiscMode mode) {
  promiscMode = mode;
  esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
  esp_wifi_set_promiscuous(true);
}

void scanAp() {
  int found = WiFi.scanNetworks(false, true);
  selectedApIndex = -1;
  if (found < 0) {
    apCount = 0;
    Serial.print("SCANERROR:"); Serial.println(found);
    Serial.println("SCANDONE");
    return;
  }
  if (found == 0) { apCount = 0; Serial.println("SCANDONE"); return; }
  int limit = min(found, WIFI_SCAN_MAX_RESULTS);
  apCount = limit;
  for (int i = 0; i < limit; i++) {
    apList[i].ssid = WiFi.SSID(i);
    memcpy(apList[i].bssid, WiFi.BSSID(i), 6);
    apList[i].rssi = WiFi.RSSI(i);
    apList[i].channel = WiFi.channel(i);
    apList[i].encType = WiFi.encryptionType(i);
    printApEntry(i);
  }
  WiFi.scanDelete();
  Serial.println("SCANDONE");
}

void scanSta() {
  staCount = 0;
  selectedStaIndex = -1;
  startPromiscuous(PromiscMode::STA_SNIFF);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)WIFI_STA_SNIFF_SECONDS * 1000UL) delay(10);
  stopPromiscuous();
  for (int i = 0; i < staCount; i++) printStaEntry(i);
  Serial.println("SCANDONE");
}

bool runSniff(const String& modeStr) {
  PromiscMode m;
  if (modeStr == "BEACON") m = PromiscMode::SNIFF_BEACON;
  else if (modeStr == "DEAUTH") m = PromiscMode::SNIFF_DEAUTH;
  else if (modeStr == "PROBE") m = PromiscMode::SNIFF_PROBE;
  else if (modeStr == "RAW") m = PromiscMode::SNIFF_RAW;
  else if (modeStr == "MULTISSID") m = PromiscMode::SNIFF_MULTISSID;
  else if (modeStr == "PMKID") m = PromiscMode::SNIFF_PMKID;
  else if (modeStr == "SAE") m = PromiscMode::SNIFF_SAE;
  else return false;
  if (m == PromiscMode::SNIFF_MULTISSID) multiSsidCount = 0;
  startPromiscuous(m);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)WIFI_SNIFF_SECONDS * 1000UL) delay(10);
  stopPromiscuous();
  Serial.println("SNIFFDONE");
  return true;
}

void runWardrive() {
  int found = WiFi.scanNetworks(false, true);
  if (found < 0) {
    Serial.print("SCANERROR:"); Serial.println(found);
    Serial.println("WARDRIVEDONE");
    return;
  }
  if (found == 0) { Serial.println("WARDRIVEDONE"); return; }
  int limit = min(found, WIFI_WARDRIVE_MAX_RESULTS);
  for (int i = 0; i < limit; i++) {
    double lat, lon;
    bool fixed = FoxGps::getFix(&lat, &lon);
    Serial.print("WARDRIVE:ssid:\""); Serial.print(WiFi.SSID(i));
    Serial.print("\" bssid:"); Serial.print(macToString(WiFi.BSSID(i)));
    Serial.print(" ch:"); Serial.print(WiFi.channel(i));
    Serial.print(" rssi:"); Serial.print(WiFi.RSSI(i));
    Serial.print(" enc:"); Serial.print(encTypeLabel(WiFi.encryptionType(i)));
    Serial.print(" pos:");
    if (fixed) { Serial.print(lat, 6); Serial.print(","); Serial.println(lon, 6); }
    else Serial.println("NOFIX");
  }
  WiFi.scanDelete();
  Serial.println("WARDRIVEDONE");
}

void runPcapCapture() {
  promiscCounter = 0;
  startPromiscuous(PromiscMode::CAPTURE);
  unsigned long start = millis();
  unsigned long lastHop = start;
  uint8_t channel = 1;
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  while (millis() - start < (unsigned long)WIFI_PCAP_SECONDS * 1000UL &&
         promiscCounter < (uint32_t)WIFI_PCAP_MAX_FRAMES) {
    if (millis() - lastHop >= (unsigned long)WIFI_PCAP_CHANNEL_HOP_MS) {
      channel = (channel % 11) + 1;
      esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
      lastHop = millis();
    }
    delay(10);
  }
  stopPromiscuous();
  Serial.print("PCAPDONE:"); Serial.println(promiscCounter);
}

void runPingScan() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("ERROR:NOWIFI"); return; }
  uint32_t localIp   = (uint32_t)WiFi.localIP();
  uint32_t mask      = (uint32_t)WiFi.subnetMask();
  uint32_t network   = localIp & mask;
  uint32_t broadcast = network | ~mask;
  int found = 0;
  for (uint32_t host = network + 1; host < broadcast; host++) {
    if (host == localIp) continue;
    IPAddress target(host);
    WiFiClient c;
    bool up = c.connect(target, 80, WIFI_PINGSCAN_TIMEOUT_MS);
    if (!up) { c.stop(); up = c.connect(target, 443, WIFI_PINGSCAN_TIMEOUT_MS); }
    if (up) {
      Serial.print("PINGUP:"); Serial.println(target.toString());
      found++;
      c.stop();
    }
    if (found >= WIFI_PINGSCAN_MAX_HOSTS) break;
    delay(1);
  }
  Serial.print("PINGSCANDONE:"); Serial.println(found);
}

void runArpScan() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("ERROR:NOWIFI"); return; }
  uint32_t localIp   = (uint32_t)WiFi.localIP();
  uint32_t mask      = (uint32_t)WiFi.subnetMask();
  uint32_t network   = localIp & mask;
  uint32_t broadcast = network | ~mask;
  WiFiUDP udp;
  int probed = 0;
  uint8_t probe = 0;
  for (uint32_t host = network + 1; host < broadcast && probed < WIFI_ARPSCAN_MAX_HOSTS; host++) {
    if (host == localIp) continue;
    IPAddress target(host);
    udp.beginPacket(target, 9);
    udp.write(probe);
    udp.endPacket();
    probed++;
    delay(WIFI_ARPSCAN_PROBE_DELAY_MS);
  }
  delay(WIFI_ARPSCAN_WAIT_MS);
  struct netif* nif = netif_default;
  int found = 0;
  if (nif) {
    for (int i = 0; i < 256; i++) {
      ip4_addr_t* ipPtr = nullptr;
      struct eth_addr* ethPtr = nullptr;
      struct netif* entryNif = nif;
      int8_t res = etharp_get_entry(i, &ipPtr, &entryNif, &ethPtr);
      if (res < 0) break;
      if (res == 0) continue;
      if (!ipPtr || !ethPtr) continue;
      char ipBuf[16];
      ip4addr_ntoa_r(ipPtr, ipBuf, sizeof(ipBuf));
      Serial.print("ARP:"); Serial.print(ipBuf);
      Serial.printf(" mac:%02X:%02X:%02X:%02X:%02X:%02X\n",
                    ethPtr->addr[0], ethPtr->addr[1], ethPtr->addr[2],
                    ethPtr->addr[3], ethPtr->addr[4], ethPtr->addr[5]);
      found++;
    }
  }
  Serial.print("ARPDONE:"); Serial.println(found);
}

void runMacTrack() {
  macTrackCount = 0;
  startPromiscuous(PromiscMode::MACTRACK);
  uint8_t channel = 1;
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  unsigned long start = millis();
  unsigned long lastHop = start;
  int lastReported = 0;
  while (millis() - start < (unsigned long)WIFI_MACTRACK_SECONDS * 1000UL) {
    if (millis() - lastHop >= (unsigned long)WIFI_PCAP_CHANNEL_HOP_MS) {
      channel = (channel % 11) + 1;
      esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
      lastHop = millis();
    }
    int n = macTrackCount;
    while (lastReported < n) {
      Serial.print("MACTRACK:"); Serial.println(macTrackList[lastReported]);
      lastReported++;
    }
    delay(20);
  }
  stopPromiscuous();
  int n = macTrackCount;
  while (lastReported < n) {
    Serial.print("MACTRACK:"); Serial.println(macTrackList[lastReported]);
    lastReported++;
  }
  Serial.print("MACTRACKDONE:"); Serial.println(macTrackCount);
}
}

namespace FoxWifiRecon {
void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
}

bool getSelectedAp(uint8_t bssidOut[6], uint8_t* channelOut, String* ssidOut) {
  if (selectedApIndex < 0 || selectedApIndex >= apCount) return false;
  memcpy(bssidOut, apList[selectedApIndex].bssid, 6);
  *channelOut = apList[selectedApIndex].channel;
  *ssidOut = apList[selectedApIndex].ssid;
  return true;
}

bool getSelectedSta(uint8_t macOut[6]) {
  if (selectedStaIndex < 0 || selectedStaIndex >= staCount) return false;
  memcpy(macOut, staList[selectedStaIndex].mac, 6);
  return true;
}

int scriptScanApCount() {
  scanAp();
  return apCount;
}

bool handleCommand(const String& line) {
  if (line == "WIFISCANAP") { scanAp(); return true; }
  if (line == "WIFISCANSTA") { scanSta(); return true; }
  if (line == "WIFISCANALL") { scanAp(); scanSta(); return true; }
  if (line == "WIFIWARDRIVE") { runWardrive(); return true; }

  if (line == "WIFILIST:AP") {
    for (int i = 0; i < apCount; i++) printApEntry(i);
    Serial.println("DONE"); return true;
  }
  if (line == "WIFILIST:STA") {
    for (int i = 0; i < staCount; i++) printStaEntry(i);
    Serial.println("DONE"); return true;
  }

  if (line.startsWith("WIFISELECT:AP:")) {
    int idx = line.substring(14).toInt();
    if (idx < 0 || idx >= apCount) Serial.println("ERROR");
    else { selectedApIndex = idx; Serial.println("OK"); }
    return true;
  }
  if (line.startsWith("WIFISELECT:STA:")) {
    int idx = line.substring(15).toInt();
    if (idx < 0 || idx >= staCount) Serial.println("ERROR");
    else { selectedStaIndex = idx; Serial.println("OK"); }
    return true;
  }
  if (line.startsWith("WIFIAPINFO:")) {
    int idx = line.substring(11).toInt();
    if (idx < 0 || idx >= apCount) Serial.println("ERROR");
    else printApEntry(idx);
    return true;
  }

  if (line == "WIFICHANNEL") {
    uint8_t channel; wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    Serial.print("CHANNEL:"); Serial.println(channel);
    return true;
  }
  if (line.startsWith("WIFICHANNEL:")) {
    int ch = line.substring(12).toInt();
    if (ch < 1 || ch > 14) Serial.println("ERROR");
    else Serial.println(esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE) == ESP_OK ? "OK" : "ERROR");
    return true;
  }

  if (line == "WIFIPACKETCOUNT") {
    promiscCounter = 0;
    startPromiscuous(PromiscMode::PACKETCOUNT);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)WIFI_PACKETCOUNT_SECONDS * 1000UL) delay(10);
    stopPromiscuous();
    Serial.print("COUNT:"); Serial.println(promiscCounter);
    return true;
  }

  if (line == "WIFISIGMON") {
    uint8_t bssid[6]; uint8_t channel; String ssid;
    if (!getSelectedAp(bssid, &channel, &ssid)) { Serial.println("ERROR"); return true; }
    memcpy(sigmonTargetBssid, bssid, 6);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    startPromiscuous(PromiscMode::SIGMON);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)WIFI_SIGMON_SECONDS * 1000UL) delay(10);
    stopPromiscuous();
    Serial.println("DONE");
    return true;
  }

  if (line == "WIFIPCAP") { runPcapCapture(); return true; }

  if (line.startsWith("WIFISNIFF:")) {
    String mode = line.substring(10); mode.trim(); mode.toUpperCase();
    if (!runSniff(mode)) Serial.println("ERROR:BADMODE");
    return true;
  }

  if (line == "WIFIMAC:GET") {
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
    Serial.print("MAC:"); Serial.println(macToString(mac));
    return true;
  }
  if (line == "WIFIMAC:RAND") {
    captureFactoryMacIfNeeded();
    uint8_t mac[6]; randomLocalMac(mac);
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err == ESP_OK) { Serial.print("MAC:"); Serial.println(macToString(mac)); }
    else Serial.println("ERROR");
    return true;
  }
  if (line.startsWith("WIFIMAC:CLONE:")) {
    captureFactoryMacIfNeeded();
    uint8_t mac[6];
    if (!macFromString(line.substring(14), mac)) { Serial.println("ERROR:BADMAC"); return true; }
    Serial.println(esp_wifi_set_mac(WIFI_IF_STA, mac) == ESP_OK ? "OK" : "ERROR");
    return true;
  }
  if (line == "WIFIMAC:CLONEAP") {
    uint8_t bssid[6]; uint8_t channel; String ssid;
    if (!getSelectedAp(bssid, &channel, &ssid)) { Serial.println("ERROR:NOAPSELECTED"); return true; }
    captureFactoryMacIfNeeded();
    Serial.println(esp_wifi_set_mac(WIFI_IF_STA, bssid) == ESP_OK ? "OK" : "ERROR");
    return true;
  }
  if (line == "WIFIMAC:RESET") {
    if (!factoryMacCaptured) { Serial.println("ERROR:NOFACTORYMAC"); return true; }
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, factoryStaMac);
    Serial.println(err == ESP_OK ? "OK" : "ERROR");
    return true;
  }

  if (line == "WIFIPINGSCAN") { runPingScan(); return true; }
  if (line == "WIFIARPSCAN") { runArpScan(); return true; }
  if (line == "WIFIMACTRACK") { runMacTrack(); return true; }

  return false;
}
}
