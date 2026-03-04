#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>

#include <Arduino_GFX_Library.h>
#include <qrcode.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <time.h>
#include <memory>
#include <vector>
#include <algorithm>

#include "mbedtls/base64.h"

#include "secrets.h" // muss definieren: MQTT_USER, MQTT_PASSWORD
#include "cert.h"    // muss definieren: ROOT_CERT_HIVEMQ (PEM-Root-CA)
// ---------------- MQTT config ----------------
static const char *MQTT_SERVER = "a5247c1ae5634398b09808b959fd47e9.s1.eu.hivemq.cloud";
static const uint16_t MQTT_PORT = 8883;
static const char *MQTT_TOPIC_SUB = "t/LeoTestID";

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

static const uint16_t MQTT_BUFFER_SIZE = 4096;
static const uint32_t HTTP_TIMEOUT_MS = 15000;
static const size_t JSON_DOC_SIZE = 4096;
static const size_t MAX_BODY_DECODED = 2048;

// ---------------- Local HTTP ingest (/meter) ----------------
static WebServer meterServer(8080); // eigener Port, kollidiert nicht mit WiFiManager-Portal
static const size_t METER_MAX_BODY = 512;

static void handleMeterPost()
{
  String body = meterServer.arg("plain"); // Body bei application/json
  IPAddress rip = meterServer.client().remoteIP();

  Serial.printf("[/meter] from=%s len=%u\n", rip.toString().c_str(), (unsigned)body.length());
  Serial.println(body);

  if (body.length() == 0 || body.length() > METER_MAX_BODY)
  {
    meterServer.send(413, "text/plain", "body too large");
    return;
  }

  // JSON nur validieren (optional, aber hilfreich)
  StaticJsonDocument<256> tmp;
  DeserializationError err = deserializeJson(tmp, body);
  if (err)
  {
    Serial.printf("[/meter] JSON error: %s\n", err.c_str());
    meterServer.send(400, "text/plain", "bad json");
    return;
  }

  meterServer.send(204, "text/plain", "");
}

static void setupMeterServer()
{
  meterServer.on("/meter", HTTP_POST, handleMeterPost);
  meterServer.on("/meter", HTTP_GET, []()
                 { meterServer.send(200, "text/plain", "ok"); }); // optional health-check
  meterServer.begin();
  Serial.println("Meter HTTP server started on :8080 (POST /meter)");
}

// ---------------- Display (Waveshare ESP32-C6-LCD-1.47) ----------------
static constexpr int PIN_MOSI = 6;
static constexpr int PIN_SCLK = 7;
static constexpr int PIN_CS = 14;
static constexpr int PIN_DC = 15;
static constexpr int PIN_RST = 21;
static constexpr int PIN_BL = 22;

Arduino_DataBus *bus = new Arduino_HWSPI(PIN_DC, PIN_CS);
// ST7789 172x320, X-Offset 34
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_RST, 0, true, 172, 320, 34, 0, 34, 0);

// ---------------- WiFiManager / Portal ----------------
WiFiManager wm;
static const char *SETUP_AP_SSID = "SmartVita Zentrale Setup";
static const char *SETUP_AP_PASS = "smartvita-setup-123"; // ändern! (>=8 Zeichen)

// ---------------- State ----------------
String espId; // "esp-<mac>"
String pendingMsg;
volatile bool hasPending = false;

// ---------------- Helpers: Display ----------------
// Zentrierten Text (eine Zeile) zeichnen
void drawCenteredText(const char *text, int y, uint16_t color = 0x0000, uint8_t size = 2)
{
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setTextSize(size);
  gfx->setTextColor(color, 0xFFFF);
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (gfx->width() - w) / 2;
  gfx->setCursor(x, y);
  gfx->print(text);
}

// Zwei Zeilen untereinander zentriert (mit anpassbarem Start-Y)
void drawCenteredTwoLines(const char *line1, const char *line2, uint16_t color = 0x0000, uint8_t size = 2, int startY = 10)
{
  int16_t x1, y1;
  uint16_t w1, h1, w2, h2;
  gfx->setTextSize(size);
  gfx->setTextColor(color, 0xFFFF);

  // Maße für Zeile 1
  gfx->getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);

  // Maße für Zeile 2
  gfx->getTextBounds(line2, 0, 0, &x1, &y1, &w2, &h2);

  // X-Positionen zentriert
  int16_t xLine1 = (gfx->width() - w1) / 2;
  int16_t xLine2 = (gfx->width() - w2) / 2;

  // Zeile 1
  gfx->setCursor(xLine1, startY);
  gfx->print(line1);

  // Zeile 2 (h1 + 4px Abstand)
  gfx->setCursor(xLine2, startY + h1 + 4);
  gfx->print(line2);
}

static void drawTopText(const char *line1, const char *line2 = nullptr)
{
  gfx->setTextSize(1);
  gfx->setTextColor(0x0000, 0xFFFF);
  gfx->setCursor(4, 4);
  gfx->println(line1);
  if (line2)
    gfx->println(line2);
}

static void drawQR(const char *text)
{
  const uint8_t version = 6; // bei längeren Strings erhöhen
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(version)];
  qrcode_initText(&qr, qrData, version, ECC_LOW, text);

  const int border = 4;
  const int modules = qr.size;
  const int total = modules + 2 * border;

  int scale = min(gfx->width() / total, gfx->height() / total);
  if (scale < 1)
    scale = 1;

  const int x0 = (gfx->width() - total * scale) / 2;
  const int y0 = (gfx->height() - total * scale) / 2;

  gfx->fillScreen(0xFFFF); // weiß

  for (int y = 0; y < modules; y++)
  {
    for (int x = 0; x < modules; x++)
    {
      if (qrcode_getModule(&qr, x, y))
      {
        gfx->fillRect(x0 + (x + border) * scale,
                      y0 + (y + border) * scale,
                      scale, scale, 0x0000);
      }
    }
  }
}

// Wi-Fi QR escaping
static String escapeWifiQr(String s)
{
  s.replace("\\", "\\\\");
  s.replace(";", "\\;");
  s.replace(":", "\\:");
  s.replace(",", "\\,");
  s.replace("\"", "\\\"");
  return s;
}

// ---------------- Helpers: Time (für TLS) ----------------
static void syncTimeOrWarn()
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 1700000000 && (millis() - start) < 15000)
  {
    delay(200);
    now = time(nullptr);
  }

  if (now < 1700000000)
  {
    Serial.println("WARN: NTP Zeit nicht gesetzt -> TLS Zertifikatsprüfung kann scheitern.");
  }
  else
  {
    Serial.println("Zeit via NTP gesetzt.");
  }
}

// ---------------- Helpers: base64 ----------------
static bool b64DecodeToString(const char *in, String &out, size_t maxOut)
{
  out = "";
  if (!in)
    return true;
  size_t inLen = strlen(in);
  if (inLen == 0)
    return true;

  size_t bufLen = (inLen * 3) / 4 + 8;
  if (bufLen > maxOut)
    return false;

  std::unique_ptr<uint8_t[]> buf(new uint8_t[bufLen]);
  size_t outLen = 0;

  int rc = mbedtls_base64_decode(buf.get(), bufLen, &outLen,
                                 (const unsigned char *)in, inLen);
  if (rc != 0)
    return false;

  out.reserve(outLen);
  for (size_t i = 0; i < outLen; i++)
    out += (char)buf[i];
  return true;
}

static String b64Encode(const uint8_t *data, size_t len)
{
  if (!data || len == 0)
    return "";
  size_t outLen = 0;
  size_t bufLen = 4 * ((len + 2) / 3) + 1;

  std::unique_ptr<uint8_t[]> buf(new uint8_t[bufLen]);
  int rc = mbedtls_base64_encode(buf.get(), bufLen, &outLen, data, len);
  if (rc != 0)
    return "";

  return String((char *)buf.get()).substring(0, outLen);
}

// ---------------- Helpers: publish ----------------
static void publishJson(const JsonDocument &doc)
{
  String out;
  serializeJson(doc, out);
  mqttClient.publish(MQTT_TOPIC_SUB, out.c_str());
}

static void publishError(const char *reqId, const char *err, int status = -1)
{
  StaticJsonDocument<768> res;
  res["v"] = 1;
  res["type"] = "res";
  res["id"] = reqId ? reqId : "";
  res["from"] = espId;
  res["ok"] = false;
  res["status"] = status;
  res["error"] = err ? err : "error";
  res["body_b64"] = "";
  publishJson(res);
}

// ---------------- allowlist (Sicherheit) ----------------
static bool isUrlAllowed(const String &url)
{
  // if (!url.startsWith("http://"))
  //   return false;
  // if (url.startsWith("http://192.168."))
  //   return true;
  // if (url.startsWith("http://10."))
  //   return true;
  return true;
}

// ---------------- HTTP execute (LAN only, per allowlist) ----------------
static bool doHttpRequest(JsonObjectConst http, int &statusOut, String &respOut)
{
  statusOut = -1;
  respOut = "";

  const char *method = http["method"] | "";
  const char *urlC = http["url"] | "";
  const char *bodyB64 = http["body_b64"] | "";

  if (method[0] == '\0' || urlC[0] == '\0')
  {
    respOut = "Missing method/url";
    return false;
  }

  String url(urlC);
  if (!isUrlAllowed(url))
  {
    respOut = "URL not allowed";
    return false;
  }

  String body;
  if (!b64DecodeToString(bodyB64, body, MAX_BODY_DECODED))
  {
    respOut = "Body base64 decode failed/too large";
    return false;
  }

  HTTPClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);

  WiFiClient net; // unverschlüsseltes HTTP im LAN
  if (!client.begin(net, url))
  {
    respOut = "HTTP begin() failed";
    return false;
  }

  // optionale Header
  if (http["headers"].is<JsonObjectConst>())
  {
    JsonObjectConst headersObj = http["headers"].as<JsonObjectConst>();
    for (JsonPairConst kv : headersObj)
    {
      const char *k = kv.key().c_str();
      const char *v = kv.value().as<const char *>();
      if (k && v)
        client.addHeader(k, v);
    }
  }

  String m(method);
  m.toUpperCase();

  int code = -1;
  String bodyResp;

  if (m == "GET")
  {
    code = client.GET();
    bodyResp = client.getString();
  }
  else if (m == "POST")
  {
    if (!(http["headers"].is<JsonObjectConst>() &&
          http["headers"].as<JsonObjectConst>().containsKey("Content-Type")))
    {
      client.addHeader("Content-Type", "application/json");
    }
    code = client.POST((uint8_t *)body.c_str(), body.length());
    bodyResp = client.getString();
  }
  else if (m == "PUT")
  {
    if (!(http["headers"].is<JsonObjectConst>() &&
          http["headers"].as<JsonObjectConst>().containsKey("Content-Type")))
    {
      client.addHeader("Content-Type", "application/json");
    }
    code = client.PUT((uint8_t *)body.c_str(), body.length());
    bodyResp = client.getString();
  }
  else if (m == "DELETE")
  {
    code = client.sendRequest("DELETE", (uint8_t *)body.c_str(), body.length());
    bodyResp = client.getString();
  }
  else
  {
    client.end();
    respOut = "Unsupported HTTP method";
    return false;
  }

  statusOut = code;
  respOut = bodyResp;

  client.end();
  return (code > 0);
}

// ---------------- MQTT request handler ----------------
static void handleReq(const JsonDocument &req)
{
  JsonObjectConst root = req.as<JsonObjectConst>();
  if (root.isNull())
    return;

  const char *id = root["id"] | "";
  if (id[0] == '\0')
    return;

  if (!root["http"].is<JsonObjectConst>())
  {
    publishError(id, "Missing http object");
    return;
  }

  JsonObjectConst httpObj = root["http"].as<JsonObjectConst>();

  int httpStatus = -1;
  String httpBody;

  bool ok = doHttpRequest(httpObj, httpStatus, httpBody);
  if (!ok)
  {
    publishError(id, httpBody.c_str(), httpStatus);
    return;
  }

  DynamicJsonDocument res(JSON_DOC_SIZE);
  res["v"] = 1;
  res["type"] = "res";
  res["id"] = id;
  res["from"] = espId;
  res["ok"] = true;
  res["status"] = httpStatus;

  String bodyB64 = b64Encode((const uint8_t *)httpBody.c_str(), httpBody.length());
  res["body_b64"] = bodyB64;

  publishJson(res);
}

// ---------------- MQTT callback (queue only) ----------------
static void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  (void)topic;

  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];
  msg.trim();

  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError err = deserializeJson(doc, msg);
  if (err)
    return;

  const char *type = doc["type"] | "";
  const char *from = doc["from"] | "";

  if (strcmp(type, "req") != 0)
    return;
  if (from[0] != '\0' && espId == String(from))
    return;

  pendingMsg = msg;
  hasPending = true;
}

// ---------------- MQTT setup/loop ----------------
static void setupMqtt()
{
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  bool resized = mqttClient.setBufferSize(MQTT_BUFFER_SIZE); // returns bool [web:149]
  Serial.print("MQTT buffer resized: ");
  Serial.println(resized ? "OK" : "FAILED");

  while (!mqttClient.connected())
  {
    Serial.print("Verbinde mit MQTT-Broker ... ");
    String clientId = "esp32c6-";
    clientId += WiFi.macAddress();

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD))
    {
      Serial.println("verbunden");
      mqttClient.subscribe(MQTT_TOPIC_SUB);
      Serial.print("Subscribed auf: ");
      Serial.println(MQTT_TOPIC_SUB);
    }
    else
    {
      Serial.print("fehlgeschlagen, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" -> neuer Versuch in 5s");
      delay(5000);
    }
  }
}

static void handleMqtt()
{
  if (!mqttClient.connected())
    setupMqtt();
  mqttClient.loop();
}

// ---------------- Portal Frontend + API ----------------
static const char ONBOARD_HTML[] PROGMEM = R"HTML(
<!doctype html><html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP Onboarding</title>
  <style>
    body{font-family:system-ui,Arial;margin:16px}
    button,input,select{font-size:16px;padding:10px;width:100%;margin:6px 0}
    pre{background:#f6f6f6;padding:10px;white-space:pre-wrap}
  </style>
</head>
<body>
  <h2>ESP Onboarding</h2>
  <button id="scan">WLANs scannen</button>
  <select id="ssid"></select>
  <input id="pass" type="password" placeholder="WLAN Passwort">
  <button id="go">Verbinden</button>
  <pre id="log"></pre>

<script>
const logEl = document.getElementById('log');
const log = (t) => { logEl.textContent += t + "\n"; };

async function scan(){
  log("Scan...");
  const r = await fetch("/api/scan", {cache:"no-store"});
  const j = await r.json();
  const sel = document.getElementById("ssid");
  sel.innerHTML = "";
  (j.aps || []).forEach(ap => {
    const o = document.createElement("option");
    o.value = ap.ssid;
    o.textContent = `${ap.ssid} (${ap.rssi} dBm)`;
    sel.appendChild(o);
  });
  log(`Found ${(j.aps||[]).length} APs`);
}

async function provision(){
  const ssid = document.getElementById("ssid").value;
  const pass = document.getElementById("pass").value;

  log("Connecting...");
  const body = new URLSearchParams({ssid, pass});
  const r = await fetch("/api/provision", {
    method:"POST",
    headers: {"Content-Type":"application/x-www-form-urlencoded"},
    body
  });
  const j = await r.json();
  log(JSON.stringify(j, null, 2));
  if (j.ok) log("Device will reboot.");
}

document.getElementById("scan").onclick = scan;
document.getElementById("go").onclick = provision;
scan();
</script>
</body></html>
)HTML";

static void handleOnboardingPage()
{
  wm.server->send_P(200, "text/html", ONBOARD_HTML);
}

static void handleScan()
{
  WiFi.mode(WIFI_AP_STA);
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, true);

  struct AP
  {
    String ssid;
    int32_t rssi;
    wifi_auth_mode_t enc;
  };
  std::vector<AP> aps;
  aps.reserve(n);

  for (int i = 0; i < n; i++)
  {
    AP ap{WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i)};
    if (ap.ssid.length())
      aps.push_back(ap);
  }
  std::sort(aps.begin(), aps.end(), [](const AP &a, const AP &b)
            { return a.rssi > b.rssi; });

  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("aps");
  for (auto &ap : aps)
  {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = ap.ssid;
    o["rssi"] = ap.rssi;
    o["open"] = (ap.enc == WIFI_AUTH_OPEN);
  }

  String out;
  serializeJson(doc, out);
  wm.server->send(200, "application/json", out);
}

static void handleProvision()
{
  String ssid = wm.server->arg("ssid");
  String pass = wm.server->arg("pass");

  DynamicJsonDocument res(768);

  if (ssid.isEmpty())
  {
    res["ok"] = false;
    res["error"] = "missing ssid";
  }
  else
  {
    // wichtig: AP aktiv lassen, sonst reißt die HTTP-Verbindung ab
    WiFi.mode(WIFI_AP_STA);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);

    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
    {
      delay(200);
    }

    bool ok = (WiFi.status() == WL_CONNECTED);
    res["ok"] = ok;
    res["status"] = (int)WiFi.status();
    if (ok)
      res["ip"] = WiFi.localIP().toString();
  }

  String out;
  serializeJson(res, out);
  wm.server->send(200, "application/json", out);

  if (res["ok"] == true)
  {
    delay(800);
    ESP.restart();
  }
}

static String portalUrl()
{
  // SoftAP-IP ist die richtige Portal-IP im Config-Mode
  return String("http://") + WiFi.softAPIP().toString() + "/";
}

static void bindServerCallback()
{
  wm.server->on("/", HTTP_GET, handleOnboardingPage);
  wm.server->on("/espOnboarding", HTTP_GET, handleOnboardingPage);

  wm.server->on("/api/scan", HTTP_GET, handleScan);
  wm.server->on("/api/provision", HTTP_POST, handleProvision);

  // ANDROID captive portal checks
  wm.server->on("/generate_204", HTTP_GET, []()
                {
    wm.server->sendHeader("Location", portalUrl(), true);
    wm.server->send(302, "text/plain", ""); });
  // manche Geräte rufen auch diese Variante auf
  wm.server->on("/generate204", HTTP_GET, []()
                {
    wm.server->sendHeader("Location", portalUrl(), true);
    wm.server->send(302, "text/plain", ""); });

  // WINDOWS captive portal check
  wm.server->on("/fwlink", HTTP_GET, []()
                {
    wm.server->sendHeader("Location", portalUrl(), true);
    wm.server->send(302, "text/plain", ""); });

  wm.server->on("/hotspot-detect.html", HTTP_GET, handleOnboardingPage);
  wm.server->on("/ncsi.txt", HTTP_GET, []()
                { wm.server->send(204, "text/plain", ""); });

  wm.server->onNotFound(handleOnboardingPage);
}

static void configModeCallback(WiFiManager *myWiFiManager)
{
  Serial.println("Entered config mode");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());

  // Wi-Fi QR payload: WIFI:T:WPA;S:<ssid>;P:<pass>;; [web:122]
  String portalSsid = myWiFiManager->getConfigPortalSSID();
  String payload = "WIFI:T:WPA;S:" + escapeWifiQr(portalSsid) +
                   ";P:" + escapeWifiQr(String(SETUP_AP_PASS)) + ";;";

  drawQR(payload.c_str());
  // Text oben (ca. 20px unter Display-Top)
  gfx->fillRect(0, 0, gfx->width(), 60, 0xFFFF);                        // weißer Bereich oben
  drawCenteredTwoLines("Scan WiFi-QR", "zum Verbinden", 0x0000, 2, 20); // y=20 Start
}

// ---------------- setup/loop ----------------
void setup()
{
  Serial.begin(115200);

  // ---------------- Backlight: dauerhaft dimmen ----------------
  // WICHTIG: Danach PIN_BL nie wieder mit pinMode()/digitalWrite() anfassen!
  static const uint8_t BL = 35; // 0..255 (kleiner = dunkler) -> z.B. 20..80 testen
  bool blOk1 = ledcAttach(PIN_BL, 5000, 8);
  bool blOk2 = ledcWrite(PIN_BL, BL);
  Serial.printf("Backlight PWM attach=%d write=%d duty=%u\n", blOk1, blOk2, BL);

  // ---------------- ID ----------------
  espId = "esp-";
  espId += WiFi.macAddress();

  // ---------------- Display init ----------------
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  gfx->begin();
  gfx->fillScreen(0xFFFF);
  drawCenteredText("Booting...", (gfx->height() - 16) / 2, 0x0000, 2);

  // optional: Debug Events
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
               {
                 (void)info;
                 Serial.printf("WiFi event: %d\n", event); });

  WiFi.mode(WIFI_STA);

  // ---------------- WiFiManager ----------------
  wm.setAPCallback(configModeCallback);
  wm.setWebServerCallback(bindServerCallback);

  // Für Tests: gespeicherte Credentials löschen
  // wm.resetSettings();

  bool ok = wm.autoConnect(SETUP_AP_SSID, SETUP_AP_PASS);
  if (!ok)
  {
    Serial.println("Provisioning fehlgeschlagen -> reboot");
    delay(1000);
    ESP.restart();
  }

  Serial.println("WLAN verbunden!");
  Serial.println(WiFi.localIP());
  setupMeterServer();

  // ---------------- Erfolgsscreen ----------------
  gfx->fillScreen(0xFFFF);

  int centerX = gfx->width() / 2;
  int centerY = 80;
  int checkSize = 40;

  gfx->fillCircle(centerX, centerY, checkSize / 2 + 2, 0x07E0);
  gfx->drawCircle(centerX, centerY, checkSize / 2 + 2, 0x06E0);
  gfx->fillCircle(centerX, centerY, checkSize / 2 - 8, 0xFFFF);

  gfx->drawLine(centerX - 12, centerY - 2, centerX - 6, centerY + 6, 0x05E0);
  gfx->drawLine(centerX - 6, centerY + 6, centerX + 10, centerY - 8, 0x05E0);

  drawCenteredTwoLines("WiFi verbunden",
                       WiFi.localIP().toString().c_str(),
                       0x0000, 2, 140);

  // Wenn du willst, kannst du hier später dimmen/ausmachen:
  // ledcWrite(PIN_BL, 0);   // aus
  // ledcWrite(PIN_BL, 35);  // wieder an (dim)

  syncTimeOrWarn();

  // TLS CA fürs MQTTS
  espClient.setCACert(ROOT_CERT_HIVEMQ);

  setupMqtt();
}

void loop()
{
  handleMqtt();
  meterServer.handleClient();

  if (hasPending)
  {
    hasPending = false;

    DynamicJsonDocument doc(JSON_DOC_SIZE);
    DeserializationError err = deserializeJson(doc, pendingMsg);
    if (!err)
      handleReq(doc);

    pendingMsg = "";
  }

  delay(10);
}
