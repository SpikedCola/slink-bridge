// S-Link / Control A1 bridge for Sony receivers.
// Bus timing from robho/sony_slink (ESP8266), ported to ESP32 with an HTTP +
// TCP interface and a control page for the STR-DB930.
//   https://github.com/robho/sony_slink
// Commands and replies from the boehmel.de S-Link page (archived):
//   http://web.archive.org/web/20180831072659/http://boehmel.de/slink.htm
//
// Bus: single wire, pulled up to ~5 V by the receiver, active low.
// A message is a 2400 us low sync, then bytes MSB first: 1200 us low = 1,
// 600 us low = 0, each low followed by 600 us released.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include "board.h"
#include "page.h"
#include "secrets.h"

static const char *HOSTNAME = "slink";

// Both directions use the RMT peripheral (pins and channels in board.h), so
// pulse timing is handled in hardware and WiFi activity on these single-core
// chips can't disturb it. TX gets one block and the driver refills it for
// longer messages.
static const size_t MAX_MSG_BYTES = 16;

static const uint32_t SYNC_US = 2400, ONE_US = 1200, ZERO_US = 600, GAP_US = 600;
static const uint32_t BIT_THRESHOLD_US = 900, SYNC_THRESHOLD_US = 2000;
static const uint16_t RX_END_US = 5000;  // bus high this long ends a capture
static const uint32_t IDLE_MS = 20;      // quiet time required before sending
static const uint32_t SEND_SPACING_MS = 300;  // leave room for the reply
static const uint32_t NO_REPLY_SPACING_MS = 60;
static const uint32_t POLL_MS = 30000;        // re-query status this often

static RingbufHandle_t rxRing;
static String lastTxHex;  // our own transmission, captured back by RX
static uint32_t lastTxMs = 0;

// ---- receiver state, built from what the receiver says on the bus ----

// Codes the receiver reports when each input is selected (03, 15, 17 are
// accepted too but report as 04, 19, 16). Listed in front-panel-ish order.
static const uint8_t INPUT_CODES[] = {0x16, 0x19, 0x10, 0x11, 0x12, 0x04, 0x05, 0x02, 0x00, 0x01};
// Every code worth asking a name for, including 2nd-audio-only ones.
static const uint8_t NAME_CODES[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11, 0x12, 0x16, 0x19};

struct ReceiverState {
  bool known = false;  // a source status has been received
  bool power = false, mute = false, multi = false, tapeLoop = false;
  int audio = -1, video = -1, second = -1;
  String inputType;  // raw 43 reply, e.g. "0001"
  String model;
  String names[0x20];
  String tuner, tunerName, surround, rearVolume;  // raw hex of last status
  uint32_t updatedMs = 0;
} rs;

// ---- log: numbered ring of recent lines so clients can poll with ?since= ----

static const int LOG_SIZE = 200;
static String logLines[LOG_SIZE];
static uint32_t logSeq = 0;
static bool debugPulses = false;

static WebServer http(80);
static WiFiServer tcpServer(23);
static WiFiClient tcpClient;

static void logLine(const String &text) {
  String line = String(millis()) + " " + text;
  logSeq++;
  logLines[logSeq % LOG_SIZE] = line;
  if (Serial) Serial.println(line);  // USB CDC: skip when no host is listening
  if (tcpClient && tcpClient.connected()) tcpClient.println(line);
}

// ---- send queue: every transmission goes through here, spaced for replies ----

static const int QUEUE_SIZE = 48;
static String sendQueue[QUEUE_SIZE];
static int queueHead = 0, queueTail = 0;

static bool parseHex(String hex, uint8_t *bytes, size_t &n) {
  hex.trim();
  n = hex.length() / 2;
  if (hex.length() == 0 || hex.length() % 2 || n > MAX_MSG_BYTES) return false;
  for (size_t i = 0; i < n; i++) {
    char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char *end;
    bytes[i] = strtoul(pair, &end, 16);
    if (*end) return false;
  }
  return true;
}

static bool enqueue(String hex) {
  hex.trim();
  hex.toLowerCase();
  uint8_t bytes[MAX_MSG_BYTES];
  size_t n;
  if (!parseHex(hex, bytes, n)) {
    logLine("!! bad hex: '" + hex + "'");
    return false;
  }
  int next = (queueTail + 1) % QUEUE_SIZE;
  if (next == queueHead) {
    logLine("!! send queue full, dropped: " + hex);
    return false;
  }
  sendQueue[queueTail] = hex;
  queueTail = next;
  return true;
}

static void queueStatusQueries() {
  enqueue("c00f");  // source, power, mute, 5.1
  enqueue("c00e");  // 2nd audio
  enqueue("c043");  // input type
}

static void queueDiscovery() {
  enqueue("c06a");  // model name
  for (uint8_t code : NAME_CODES) {
    char hex[7];
    snprintf(hex, sizeof hex, "c048%02x", code);
    enqueue(hex);
  }
  queueStatusQueries();
}

// ---- decode receiver messages into state ----

static String asciiField(const uint8_t *b, size_t n) {
  String s;
  for (size_t i = 0; i < n && b[i]; i++) s += (char)b[i];
  s.trim();
  return s;
}

static void onMessage(const String &hex) {
  uint8_t b[32];
  size_t n = hex.length() / 2;
  if (n < 2 || n > sizeof b) return;
  for (size_t i = 0; i < n; i++) b[i] = strtoul(hex.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);

  rs.updatedMs = millis();
  if (b[0] == 0xc8) {
    switch (b[1]) {
      case 0x2e:
        rs.power = true;
        queueStatusQueries();
        break;
      case 0x2f: rs.power = false; break;
      case 0x06: rs.mute = true; break;
      case 0x07: rs.mute = false; break;
      case 0x0c: rs.multi = true; break;
      case 0x0d: rs.multi = false; break;
      case 0x70:
        if (n >= 5) {
          if (rs.known && b[2] != rs.audio) enqueue("c043");  // type is per input
          rs.known = true;
          rs.audio = b[2];
          rs.video = b[3];
          rs.power = b[4] & 0x01;
          rs.mute = b[4] & 0x02;
          rs.tapeLoop = b[4] & 0x08;
          rs.multi = b[4] & 0x10;
        }
        break;
      case 0x71:
        if (n >= 3) rs.second = b[2];
        break;
      case 0x43: rs.inputType = hex.substring(4); break;
      case 0x6a: rs.model = asciiField(b + 2, n - 2); break;
      case 0x48:
        if (n >= 3 && b[2] < 0x20) rs.names[b[2]] = asciiField(b + 3, n - 3);
        break;
    }
  } else if (b[0] == 0xc9) {
    if (b[1] == 0x48) rs.tunerName = asciiField(b + 4, n > 4 ? n - 4 : 0);
    else rs.tuner = hex;
  } else if (b[0] == 0xcb) {
    if (b[1] == 0x40) rs.rearVolume = hex;
    else if (b[1] == 0x70) rs.surround = hex;
  }
}

// ---- receive: decode RMT captures into hex messages ----

// 1 tick = 1 us on both channels. TX: level 1 on the pin turns Q1 on, pulling
// the bus low. RX: each capture is one message, ended by RX_END_US of idle.
static void setupRmt() {
  rmt_config_t tx = RMT_DEFAULT_CONFIG_TX((gpio_num_t)TX_PIN, TX_RMT);
  tx.clk_div = 80;  // 80 MHz APB -> 1 us
  rmt_config(&tx);
  rmt_driver_install(TX_RMT, 0, 0);

  rmt_config_t rx = RMT_DEFAULT_CONFIG_RX((gpio_num_t)RX_PIN, RX_RMT);
  rx.clk_div = 80;
  rx.mem_block_num = RX_MEM_BLOCKS;
  rx.rx_config.idle_threshold = RX_END_US;
  rmt_config(&rx);
  rmt_driver_install(RX_RMT, 4096, 0);
#if SOC_RMT_SUPPORT_RX_PINGPONG
  // Copy out each half of RX memory as it fills, so long replies fit.
  rmt_set_rx_thr_intr_en(RX_RMT, true, RX_MEM_BLOCKS * SOC_RMT_MEM_WORDS_PER_CHANNEL / 2);
#endif
  rmt_get_ringbuf_handle(RX_RMT, &rxRing);
  rmt_rx_start(RX_RMT, true);
}

static void decodeCapture(const rmt_item32_t *items, size_t count) {
  String hex, widths;
  uint8_t byte = 0, bits = 0;
  bool synced = false;
  for (size_t i = 0; i < count; i++) {
    // Each item holds two (level, duration) halves; only low times carry data.
    const uint32_t halves[2][2] = {{items[i].level0, items[i].duration0},
                                   {items[i].level1, items[i].duration1}};
    for (auto &h : halves) {
      if (h[0] != 0 || h[1] == 0) continue;
      uint32_t width = h[1];
      if (debugPulses) widths += String(width) + " ";
      if (width > SYNC_THRESHOLD_US) {
        synced = true;
        continue;
      }
      if (!synced) continue;
      byte = (byte << 1) | (width > BIT_THRESHOLD_US ? 1 : 0);
      if (++bits == 8) {
        char b[3];
        snprintf(b, sizeof b, "%02x", byte);
        hex += b;
        byte = 0;
        bits = 0;
      }
    }
  }
  if (hex.isEmpty() && bits == 0) return;

  bool echo = hex == lastTxHex && millis() - lastTxMs < 500;
  if (echo) {
    lastTxHex = "";
  } else {
    String s = (synced ? "RX " : "RX (no sync) ") + hex;
    if (bits) s += " (+" + String(bits) + " stray bits)";
    logLine(s);
    if (synced && bits == 0) onMessage(hex);
  }
  if (debugPulses) logLine(String(echo ? "   echo" : "   ") + " pulses(us): " + widths);
}

static void processBus() {
  size_t len;
  rmt_item32_t *items;
  while ((items = (rmt_item32_t *)xRingbufferReceive(rxRing, &len, 0))) {
    decodeCapture(items, len / sizeof(rmt_item32_t));
    vRingbufferReturnItem(rxRing, items);
  }
}

// Bus has been high for IDLE_MS. Gives up after a second.
static bool waitBusIdle() {
  uint32_t start = millis(), highSince = millis();
  while (millis() - highSince < IDLE_MS) {
    if (millis() - start > 1000) return false;
    if (digitalRead(RX_PIN) == LOW) highSince = millis();
    delayMicroseconds(100);
  }
  return true;
}

// ---- transmit ----

static rmt_item32_t pulseItem(uint32_t lowUs) {
  rmt_item32_t item;
  item.level0 = 1;
  item.duration0 = lowUs;
  item.level1 = 0;
  item.duration1 = GAP_US;
  return item;
}

static bool sendHex(const String &hex) {
  uint8_t bytes[MAX_MSG_BYTES];
  size_t n;
  if (!parseHex(hex, bytes, n)) {
    logLine("!! bad hex: '" + hex + "'");
    return false;
  }

  if (!waitBusIdle()) {
    logLine("!! bus busy, not sent: " + hex);
    return false;
  }
  processBus();

  rmt_item32_t items[1 + MAX_MSG_BYTES * 8];
  size_t count = 0;
  items[count++] = pulseItem(SYNC_US);
  for (size_t i = 0; i < n; i++)
    for (int b = 7; b >= 0; b--) items[count++] = pulseItem((bytes[i] >> b) & 1 ? ONE_US : ZERO_US);

  lastTxHex = hex;
  lastTxMs = millis();
  rmt_write_items(TX_RMT, items, count, false);
  delayMicroseconds(300);  // mid-sync: the bus should read low now
  bool ok = digitalRead(RX_PIN) == LOW;
  rmt_wait_tx_done(TX_RMT, portMAX_DELAY);

  logLine("TX " + hex);
  if (!ok) logLine("!! bus never went low during sync; check Q1 and the plug");
  return ok;
}

// Volume and level steps get no reply, so they can go out back to back.
static bool expectsReply(const String &hex) {
  static const char *const NO_REPLY[] = {"c014", "c015", "c040", "c310", "c311", "c312", "c313", "c314", "c315"};
  for (const char *p : NO_REPLY)
    if (hex.startsWith(p)) return false;
  return true;
}

static void pumpQueue() {
  static uint32_t spacingMs = 0;
  if (queueHead == queueTail || millis() - lastTxMs < spacingMs) return;
  String hex = sendQueue[queueHead];
  queueHead = (queueHead + 1) % QUEUE_SIZE;
  sendHex(hex);
  spacingMs = expectsReply(hex) ? SEND_SPACING_MS : NO_REPLY_SPACING_MS;
}

// ---- command handling (TCP, serial) ----

static void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) return;
  if (cmd == "debug on" || cmd == "debug off") {
    debugPulses = cmd == "debug on";
    logLine(String("pulse debug ") + (debugPulses ? "on" : "off"));
  } else {
    enqueue(cmd);
  }
}

static void readLines(Stream &in, String &buf) {
  while (in.available()) {
    char c = in.read();
    if (c == '\n') {
      handleCommand(buf);
      buf = "";
    } else if (c != '\r' && buf.length() < 128) {
      buf += c;
    }
  }
}

static void handleTcp() {
  static String buf;
  if (tcpServer.hasClient()) {
    if (tcpClient) tcpClient.stop();
    tcpClient = tcpServer.available();
    tcpClient.println("slink bridge: type hex to send (e.g. c02e), or 'debug on' / 'debug off'");
    buf = "";
  }
  if (tcpClient) readLines(tcpClient, buf);
}

// ---- HTTP ----

static String jsonString(const String &s) {
  String out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c >= 0x20) out += c;
  }
  return out + "\"";
}

static String stateJson() {
  String j = "{";
  j += "\"known\":" + String(rs.known ? "true" : "false");
  j += ",\"power\":" + String(rs.power ? "true" : "false");
  j += ",\"mute\":" + String(rs.mute ? "true" : "false");
  j += ",\"multi\":" + String(rs.multi ? "true" : "false");
  j += ",\"tapeLoop\":" + String(rs.tapeLoop ? "true" : "false");
  j += ",\"audio\":" + String(rs.audio);
  j += ",\"video\":" + String(rs.video);
  j += ",\"second\":" + String(rs.second);
  j += ",\"inputType\":" + jsonString(rs.inputType);
  j += ",\"model\":" + jsonString(rs.model);
  j += ",\"tuner\":" + jsonString(rs.tuner);
  j += ",\"tunerName\":" + jsonString(rs.tunerName);
  j += ",\"surround\":" + jsonString(rs.surround);
  j += ",\"rearVolume\":" + jsonString(rs.rearVolume);
  j += ",\"ageMs\":" + String(rs.updatedMs ? (long)(millis() - rs.updatedMs) : -1L);
  j += ",\"queued\":" + String((queueTail - queueHead + QUEUE_SIZE) % QUEUE_SIZE);
  j += ",\"logSeq\":" + String(logSeq);
  j += ",\"inputs\":[";
  bool first = true;
  for (uint8_t code : INPUT_CODES) {
    j += String(first ? "" : ",") + "{\"code\":" + code + ",\"name\":" + jsonString(rs.names[code]) + "}";
    first = false;
  }
  j += "],\"names\":{";
  first = true;
  for (int code = 0; code < 0x20; code++) {
    if (rs.names[code].isEmpty()) continue;
    j += String(first ? "" : ",") + "\"" + code + "\":" + jsonString(rs.names[code]);
    first = false;
  }
  return j + "}}";
}

static String logSince(uint32_t since) {
  uint32_t first = logSeq >= LOG_SIZE ? logSeq - LOG_SIZE + 1 : 1;
  if (since + 1 > first) first = since + 1;
  String out;
  for (uint32_t s = first; s <= logSeq; s++) out += String(s) + " " + logLines[s % LOG_SIZE] + "\n";
  return out;
}

static void setupHttp() {
  http.on("/", [] { http.send_P(200, "text/html", PAGE); });
  http.on("/api/state", [] {
    http.sendHeader("Cache-Control", "no-store");
    http.send(200, "application/json", stateJson());
  });
  http.on("/api/log", [] { http.send(200, "text/plain", logSince(http.arg("since").toInt())); });
  http.on("/api/send", HTTP_POST, [] {
    // Several commands may be given, separated by spaces or commas.
    String all = http.arg("hex");
    all.replace(",", " ");
    bool ok = true;
    int start = 0;
    while (start < (int)all.length()) {
      int end = all.indexOf(' ', start);
      if (end < 0) end = all.length();
      String one = all.substring(start, end);
      if (one.length()) ok &= enqueue(one);
      start = end + 1;
    }
    http.send(ok ? 200 : 400, "text/plain", ok ? "queued\n" : "failed, see log\n");
  });
  http.on("/api/refresh", [] {
    queueDiscovery();
    http.send(200, "text/plain", "queued\n");
  });

  // Plain-text endpoints for curl.
  http.on("/help", [] {
    http.send(200, "text/plain",
              "GET /                  control page\n"
              "GET /api/state         receiver state (JSON)\n"
              "POST /api/send?hex=c00f queue one or more messages (space/comma separated)\n"
              "GET /api/refresh       re-read names and status\n"
              "GET /status            wifi + bus state\n"
              "GET /log?since=N       log lines after sequence N\n"
              "POST /send?hex=c02e    same as /api/send\n"
              "GET /debug?on=1|0      include raw pulse widths in the log\n"
              "TCP port 23            live log; type hex lines to send\n");
  });
  http.on("/status", [] {
    String s = "ip " + WiFi.localIP().toString() + "\n";
    s += "rssi " + String(WiFi.RSSI()) + "\n";
    s += "uptime_ms " + String(millis()) + "\n";
    s += String("bus ") + (digitalRead(RX_PIN) ? "high (idle)" : "LOW (held low / receiver off?)") + "\n";
    s += "log_seq " + String(logSeq) + "\n";
    http.send(200, "text/plain", s);
  });
  http.on("/log", [] { http.send(200, "text/plain", logSince(http.arg("since").toInt())); });
  http.on("/send", HTTP_POST, [] {
    bool ok = enqueue(http.arg("hex"));
    http.send(ok ? 200 : 400, "text/plain", ok ? "queued\n" : "failed, see /log\n");
  });
  http.on("/debug", [] {
    handleCommand(http.arg("on") == "0" ? "debug off" : "debug on");
    http.send(200, "text/plain", debugPulses ? "pulse debug on\n" : "pulse debug off\n");
  });
  http.begin();
}

// ---- main ----

void setup() {
  Serial.begin(115200);
  setupRmt();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#ifdef WIFI_TX_POWER
  WiFi.setTxPower(WIFI_TX_POWER);
#endif
  logLine("booted; connecting to " WIFI_SSID);
  queueDiscovery();
}

void loop() {
  static bool servicesStarted = false, online = false;
  static uint32_t lastPoll = 0;
  static String serialBuf;

  if (WiFi.status() == WL_CONNECTED) {
    if (!online) {
      online = true;
      logLine("online at " + WiFi.localIP().toString() + " (" + HOSTNAME + ".local)");
    }
    if (!servicesStarted) {
      servicesStarted = true;
      ArduinoOTA.setHostname(HOSTNAME);  // also starts mDNS
      ArduinoOTA.setPassword(OTA_PASS);
      ArduinoOTA.begin();
      setupHttp();
      tcpServer.begin();
    }
    ArduinoOTA.handle();
    http.handleClient();
    handleTcp();
  } else if (online) {
    online = false;
    logLine("wifi lost, reconnecting");
  }

  if (millis() - lastPoll > POLL_MS) {
    lastPoll = millis();
    enqueue("c00f");
  }

  processBus();
  pumpQueue();
  readLines(Serial, serialBuf);
}
