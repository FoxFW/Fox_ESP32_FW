#include "gps.h"
#include "config.h"

#if FOX_HAS_GPS

#include <HardwareSerial.h>

namespace {

HardwareSerial gpsSerial(2);

char nmeaBuf[GPS_NMEA_LINE_MAX];
int nmeaLen = 0;

bool gpsInitialized = false;
bool hasFix = false;
double lastLat = 0.0;
double lastLon = 0.0;
double lastAltM = 0.0;
int lastSatCount = 0;
String lastDate;
String lastTime;

struct Poi {
  double lat;
  double lon;
  String label;
};
Poi poiList[GPS_POI_MAX];
int poiCount = 0;
bool poiActive = false;

double nmeaToDecimalDegrees(const String& raw, char hemi, bool isLon) {
  int dotIdx = raw.indexOf('.');
  if (dotIdx < 2) return 0.0;
  int degDigits = isLon ? 3 : 2;
  if (dotIdx < degDigits) return 0.0;
  double deg = raw.substring(0, degDigits).toDouble();
  double minutes = raw.substring(degDigits).toDouble();
  double dec = deg + minutes / 60.0;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  return dec;
}

int splitFields(const String& body, String* out, int maxFields) {
  int count = 0;
  int start = 0;
  while (count < maxFields) {
    int comma = body.indexOf(',', start);
    if (comma < 0) {
      out[count++] = body.substring(start);
      break;
    }
    out[count++] = body.substring(start, comma);
    start = comma + 1;
  }
  return count;
}

bool checksumOk(const String& sentence) {
  int star = sentence.indexOf('*');
  if (star < 0 || star + 3 > (int)sentence.length()) return true;
  uint8_t sum = 0;
  for (int i = 1; i < star; i++) sum ^= (uint8_t)sentence[i];
  char hex[3];
  snprintf(hex, sizeof(hex), "%02X", sum);
  String want = sentence.substring(star + 1, star + 3);
  want.toUpperCase();
  return want == String(hex);
}

void parseRMC(const String& body) {

  String f[12];
  if (splitFields(body, f, 12) < 10) return;
  lastTime = f[1];
  if (f[2] != "A") {
    hasFix = false;
    return;
  }
  lastLat = nmeaToDecimalDegrees(f[3], f[4].length() ? f[4][0] : 'N', false);
  lastLon = nmeaToDecimalDegrees(f[5], f[6].length() ? f[6][0] : 'E', true);
  lastDate = f[9];
  hasFix = true;
}

void parseGGA(const String& body) {

  String f[15];
  if (splitFields(body, f, 15) < 10) return;
  int fixQuality = f[6].toInt();
  lastSatCount = f[7].toInt();
  if (fixQuality <= 0) return;
  lastLat = nmeaToDecimalDegrees(f[2], f[3].length() ? f[3][0] : 'N', false);
  lastLon = nmeaToDecimalDegrees(f[4], f[5].length() ? f[5][0] : 'E', true);
  lastAltM = f[9].toDouble();
  hasFix = true;
}

void handleSentence(const String& sentence) {
  if (sentence.length() < 6 || sentence[0] != '$') return;
  if (!checksumOk(sentence)) return;

  int star = sentence.indexOf('*');
  String body = (star < 0) ? sentence.substring(1) : sentence.substring(1, star);

  int firstComma = body.indexOf(',');
  if (firstComma < 3) return;
  String type = body.substring(0, firstComma);
  String suffix = type.substring(type.length() - 3);

  if (suffix == "RMC") parseRMC(body);
  else if (suffix == "GGA") parseGGA(body);
}

void pump() {
  while (gpsSerial.available()) {
    char c = (char)gpsSerial.read();
    if (c == '\n') {
      if (nmeaLen > 0) {
        nmeaBuf[nmeaLen] = 0;
        handleSentence(String(nmeaBuf));
      }
      nmeaLen = 0;
    } else if (c != '\r') {
      if (nmeaLen < GPS_NMEA_LINE_MAX - 1) nmeaBuf[nmeaLen++] = c;
    }
  }
}

void doInit() {
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  gpsInitialized = true;
  hasFix = false;
  nmeaLen = 0;
  Serial.println("OK");
}

}

namespace FoxGps {

void loop() {
  if (gpsInitialized) pump();
}

bool getFix(double* latOut, double* lonOut) {
  if (!gpsInitialized || !hasFix) return false;
  *latOut = lastLat;
  *lonOut = lastLon;
  return true;
}

bool handleCommand(const String& line) {
  if (line == "GPSINIT") {
    doInit();
    return true;
  }

  static const char* prefixes[] = {
    "GPS:FIX", "GPS:SAT", "GPS:LAT", "GPS:LON", "GPS:ALT", "GPS:DATE",
    "GPSTRACK", "GPSPOI:START", "GPSPOI:MARK", "GPSPOI:END"
  };
  bool matchesGpsFamily = false;
  for (auto p : prefixes) {
    if (line == p || line.startsWith(p)) { matchesGpsFamily = true; break; }
  }
  if (!matchesGpsFamily) return false;

  if (!gpsInitialized) {
    Serial.println("ERROR:NOTINIT");
    return true;
  }

  if (line == "GPS:FIX") {
    Serial.println(hasFix ? "FIX:1" : "FIX:0");
    return true;
  }

  if (line == "GPS:SAT") {
    Serial.print("SAT:");
    Serial.println(lastSatCount);
    return true;
  }

  if (line == "GPS:LAT") {
    if (!hasFix) { Serial.println("ERROR:NOFIX"); return true; }
    Serial.println(lastLat, 6);
    return true;
  }

  if (line == "GPS:LON") {
    if (!hasFix) { Serial.println("ERROR:NOFIX"); return true; }
    Serial.println(lastLon, 6);
    return true;
  }

  if (line == "GPS:ALT") {
    if (!hasFix) { Serial.println("ERROR:NOFIX"); return true; }
    Serial.println(lastAltM, 1);
    return true;
  }

  if (line == "GPS:DATE") {
    Serial.print("DATE:");
    Serial.print(lastDate);
    Serial.print(" TIME:");
    Serial.println(lastTime);
    return true;
  }

  if (line == "GPSTRACK") {
    unsigned long start = millis();
    String lastPrinted;
    while (millis() - start < (unsigned long)GPS_TRACK_SECONDS * 1000UL) {
      pump();
      if (hasFix) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%.6f,%.6f", lastLat, lastLon);
        String cur(buf);
        if (cur != lastPrinted) {
          Serial.print("FIX:");
          Serial.println(cur);
          lastPrinted = cur;
        }
      }
      delay(50);
    }
    Serial.println("TRACKDONE");
    return true;
  }

  if (line == "GPSPOI:START") {
    poiCount = 0;
    poiActive = true;
    Serial.println("OK");
    return true;
  }

  if (line == "GPSPOI:MARK" || line.startsWith("GPSPOI:MARK:")) {
    if (!poiActive) { Serial.println("ERROR:NOTSTARTED"); return true; }
    if (!hasFix) { Serial.println("ERROR:NOFIX"); return true; }
    if (poiCount >= GPS_POI_MAX) { Serial.println("ERROR:FULL"); return true; }
    String label = line.startsWith("GPSPOI:MARK:")
        ? line.substring(strlen("GPSPOI:MARK:"))
        : (String("poi") + String(poiCount));
    poiList[poiCount].lat = lastLat;
    poiList[poiCount].lon = lastLon;
    poiList[poiCount].label = label;
    poiCount++;
    Serial.print("POI:");
    Serial.print(poiCount - 1);
    Serial.print(" ");
    Serial.print(label);
    Serial.print(" ");
    Serial.print(lastLat, 6);
    Serial.print(",");
    Serial.println(lastLon, 6);
    return true;
  }

  if (line == "GPSPOI:END") {
    poiActive = false;
    Serial.print("POICOUNT:");
    Serial.println(poiCount);
    return true;
  }

  return false;
}

}

#else

namespace FoxGps {

void loop() {}

bool getFix(double* latOut, double* lonOut) {
  (void)latOut;
  (void)lonOut;
  return false;
}

bool handleCommand(const String& line) {
  static const char* prefixes[] = {
    "GPSINIT", "GPS:FIX", "GPS:SAT", "GPS:LAT", "GPS:LON", "GPS:ALT", "GPS:DATE",
    "GPSTRACK", "GPSPOI:START", "GPSPOI:MARK", "GPSPOI:END"
  };
  for (auto p : prefixes) {
    if (line == p || line.startsWith(p)) {
      Serial.println("ERROR:NOGPS");
      return true;
    }
  }
  return false;
}

}

#endif
