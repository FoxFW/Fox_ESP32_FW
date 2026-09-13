#pragma once

#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define FOX_HAS_BLE 0
#else
#define FOX_HAS_BLE 1
#endif

#if FOX_HAS_BLE && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6))
#define FOX_BLE_NIMBLE 1
#else
#define FOX_BLE_NIMBLE 0
#endif

#define FOX_HAS_RFID 0
#define FOX_HAS_SUBGHZ 0
#define FOX_HAS_IR 0
#define FOX_HAS_GPS 0

// FoxLAB (the FoxLAB WiFi AP + captive web portal toggled by the Flipper's
// "FoxLAB" app - see fox_lab.cpp) is a WiFi AP + a single plain-HTTP
// WebServer, with its page living in flash via PROGMEM (not RAM). That's
// a lighter version of what fox_csi.cpp's web UI already runs on every
// board including S2 - AP + WebServer + a WebSocketsServer on top - with
// no gate at all, so FoxLAB doesn't add a new category of S2 RAM risk.
// S2 gate is OPEN below. It stays here, easy to flip back to 0 for S2
// only, in case on-device testing says otherwise - in particular, FoxLAB
// running at the same time as anything that does a TLS handshake
// (Discord/GitHub/Gemini's HTTPS clients - the actual RAM ceiling that's
// been hit on S2, being tracked separately) is the case worth watching,
// since that eats into the same free-heap pool.
#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define FOX_HAS_LAB 1
#else
#define FOX_HAS_LAB 1
#endif

#define SERIAL_BAUD 115200
#define LINE_BUFFER_MAX 2200
#define HEX_BUFFER_MAX 256

// Default max is ~19.5dBm. WiFi TX bursts (especially combined with TLS
// handshake CPU load) can pull enough current to brown out a board on a
// marginal power supply - trading a bit of range for a lower peak current
// draw fixes that on the software side. WIFI_POWER_15dBm, not any lower,
// to keep recon/attack range from taking too much of a hit.
#define FOX_WIFI_TX_POWER WIFI_POWER_15dBm

#define FOX_FIRMWARE_VERSION "1.3.0"

#define BLE_SCAN_SECONDS 5

#define WIFI_SCAN_MAX_RESULTS 40
#define WIFI_STA_SCAN_MAX_RESULTS 40
#define WIFI_STA_SNIFF_SECONDS 8
#define WIFI_SIGMON_SECONDS 10
#define WIFI_PACKETCOUNT_SECONDS 5

#define WIFI_SNIFF_SECONDS 8
#define WIFI_SNIFF_RESULTS_MAX 30
#define WIFI_SNIFF_MULTISSID_SSID_MAX 16
#define WIFI_SNIFF_MULTISSID_THRESHOLD 8

#define ATTACK_BURST_SECONDS 5
#define ATTACK_PACKET_INTERVAL_MS 5
#define BEACON_SPAM_SSID_MAX 16
#define BEACON_SPAM_SSID_LEN_MAX 32

#define WIFI_RICKROLL_SSID_COUNT 8
#define WIFI_CSA_TARGET_CHANNEL 6

#define WIFI_PORTSCAN_MAX_PORTS 100
#define WIFI_PORTSCAN_TIMEOUT_MS 250

#define FOX_PORTAL_HTML_MAX 1024
#define FOX_PORTAL_DEFAULT_SSID "Fox Portal Demo"
#define FOX_PORTAL_FIELD_MAX 64
#define FOX_PORTAL_MAX_FIELDS 12
#define FOX_PORTAL_KEY_MAX 16
#define FOX_PORTAL_TITLE_MAX 48
#define FOX_PORTAL_INTRO_MAX 200
#define FOX_PORTAL_NOTE_MAX 200

#define BLESPAM_BURST_SECONDS 8
#define BLESPAM_ADV_INTERVAL_MS 100

#define WIFI_SAVED_MAX 5
#define HTTP_BODY_MAX 2048
#define HTTP_TIMEOUT_MS 10000
#define DOWNLOAD_HTTP_TIMEOUT_MS 25000
#define DOWNLOAD_MAX_REDIRECTS 5
#define DOWNLOAD_TLS_HANDSHAKE_TIMEOUT_SEC 18
#define HTTP_BYTES_MAX 3072

#define DOWNLOAD_STREAM_FRAME 1024
#define FAST_BAUD_RATE 921600

#define WEBSOCKET_RECONNECT_MS 5000

#define DISCORD_READ_LIMIT_DEFAULT 5
#define DISCORD_READ_LIMIT_MAX 10
#define DISCORD_CONTENT_PREVIEW_MAX 100
#define DISCORD_POST_MIN_INTERVAL_MS 3000

#define FOXCHAT_RELAY_BASE_URL "https://foxfw-chat-relay.foxcustomfirmware.workers.dev"
#define FOXCHAT_RELAY_APP_KEY  "foxfw-esp32-chat-v1"

#define GEMINI_API_TIMEOUT_MS 20000
#define GEMINI_RATELIMIT_COOLDOWN_SEC 60

/* Set these to match your deployed Cloudflare Worker (see
 * cloudflare_worker/gemini_relay.js) - GEMINI_RELAY_APP_KEY just needs to
 * match the Worker's APP_KEY secret exactly, it isn't a real credential. */
#define GEMINI_RELAY_BASE_URL "https://foxfw-gemini-relay.foxcustomfirmware.workers.dev"
#define GEMINI_RELAY_APP_KEY  "foxfw-esp32-gemini-v1"

// Only g_tokens/g_arrays/g_objects (below) are actually reserved as static
// RAM at boot on every board, regardless of whether scripting is ever used -
// SCRIPT_SOURCE_MAX/VARS_MAX/STRING_MAX bound dynamic Strings and a
// stack-local interpreter instead, so they're left equal to every other
// board (shrinking them would risk breaking real scripts for no RAM gain).
// On the S2 (320KB SRAM vs. classic's 520KB) the three static tables at
// their full size account for ~42KB of static RAM that competes directly
// with the heap TLS needs for Fox Chat/Gemini. The limits below keep >1.5x
// headroom over a realistic multi-step automation script (measured ~150
// tokens for a WiFi-connect + heap-check + GPIO + HTTP loop) while still
// freeing a meaningful chunk of that 42KB on S2 only.
#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define SCRIPT_SOURCE_MAX 2048
#define SCRIPT_TOKENS_MAX 256
#define SCRIPT_VARS_MAX 24
#define SCRIPT_STRING_MAX 160
#define SCRIPT_CALL_ARGS_MAX 6
#define SCRIPT_NAME_MAX 32
#define SCRIPT_HTTP_GET_MAX 512

#define SCRIPT_ARRAYS_MAX 6
#define SCRIPT_ARRAY_LEN_MAX 20
#define SCRIPT_OBJECTS_MAX 6
#define SCRIPT_OBJECT_KEYS_MAX 10
#define SCRIPT_FUNCS_MAX 8
#define SCRIPT_FUNC_PARAMS_MAX 4
#define SCRIPT_CALL_DEPTH_MAX 6
#define SCRIPT_LOOP_MAX_ITER 5000
#else
#define SCRIPT_SOURCE_MAX 2048
#define SCRIPT_TOKENS_MAX 512
#define SCRIPT_VARS_MAX 24
#define SCRIPT_STRING_MAX 160
#define SCRIPT_CALL_ARGS_MAX 6
#define SCRIPT_NAME_MAX 32
#define SCRIPT_HTTP_GET_MAX 512

#define SCRIPT_ARRAYS_MAX 12
#define SCRIPT_ARRAY_LEN_MAX 32
#define SCRIPT_OBJECTS_MAX 12
#define SCRIPT_OBJECT_KEYS_MAX 16
#define SCRIPT_FUNCS_MAX 8
#define SCRIPT_FUNC_PARAMS_MAX 4
#define SCRIPT_CALL_DEPTH_MAX 6
#define SCRIPT_LOOP_MAX_ITER 5000
#endif

#define SCRIPT_STORAGE_KEY_MAX 32
#define SCRIPT_STORAGE_VALUE_MAX 128

#define PN532_IRQ_PIN 25
#define PN532_RESET_PIN 26
#define RFID_MIFARE_DEFAULT_KEY {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

#define SUBGHZ_SCK_PIN 18
#define SUBGHZ_MISO_PIN 19
#define SUBGHZ_MOSI_PIN 23
#define SUBGHZ_CS_PIN 27
#define SUBGHZ_GDO0_PIN 32
#define SUBGHZ_DEFAULT_MHZ 433.92
#define SUBGHZ_RX_BUFFER_MAX 64
#define SUBGHZ_RX_WAIT_SECONDS 10

#define IR_SEND_PIN 4
#define IR_RECV_PIN 5
#define IR_RECV_BUFFER_SIZE 1024
#define IR_RECV_GAP_TIMEOUT_MS 15
#define IR_RECV_WAIT_SECONDS 10

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD 9600
#define GPS_NMEA_LINE_MAX 128
#define GPS_POI_MAX 20
#define GPS_TRACK_SECONDS 30

#define WIFI_WARDRIVE_MAX_RESULTS WIFI_SCAN_MAX_RESULTS

#define WIFI_PCAP_SECONDS 15
#define WIFI_PCAP_SNAPLEN 256
#define WIFI_PCAP_MAX_FRAMES 150
#define WIFI_PCAP_CHUNK_BYTES 56
#define WIFI_PCAP_CHANNEL_HOP_MS 400

#define BLE_TAG_SCAN_SECONDS 8

#define WIFI_PINGSCAN_TIMEOUT_MS 200
#define WIFI_PINGSCAN_MAX_HOSTS 254
#define WIFI_ARPSCAN_MAX_HOSTS 254
#define WIFI_ARPSCAN_PROBE_DELAY_MS 5
#define WIFI_ARPSCAN_WAIT_MS 1500
#define WIFI_MACTRACK_SECONDS 10
#define WIFI_MACTRACK_MAX_MACS 64

#define WIFI_KARMA_SNIFF_SECONDS 8

#define BLE_SPOOFAT_INTERVAL_MS 500

#define STATUS_LED_PIN 2
