// Baby monitor: AI-Thinker ESP32-CAM (HW-297) + INMP441 I2S microphone.
//
// Audio and video are deliberately two independent streams:
//   :82/audio  - endless WAV, always running
//   :81/stream - MJPEG, only while somebody is watching
//   :80/       - page, /level, /set
//
// MJPEG cannot carry sound at all (it is just concatenated JPEGs), and a baby
// monitor does not need lip sync - it needs to be heard. Keeping the two apart
// also means a stalled video connection cannot silence the microphone.
//
// The camera driver owns I2S0 (it clocks the parallel DVP bus), so the
// microphone runs on I2S1. They do not collide.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <math.h>

#include "esp_camera.h"
#include "esp_http_server.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/stream_buffer.h"

#include "config.h"
#include "page.h"

// ----------------------------------------------------------------- camera pins
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ----------------------------------------------------------------- state
static StreamBufferHandle_t s_audio = nullptr;
static volatile float       s_level = 0.0f;   // 0..1 RMS of the last block
static volatile int         s_gain  = MIC_DEFAULT_GAIN;
static volatile int         s_lamp  = 0;       // 0..255

// Diagnostics. "mic ok" at boot only proves the driver installed; it says
// nothing about whether the bus actually carries data. These separate the
// three ways silence can happen: i2s_read never returning (reads stays 0),
// the data line stuck at a constant (rawMin == rawMax), or a genuinely quiet
// microphone (raw swings, level near zero).
static volatile uint32_t s_reads   = 0;
static volatile int32_t  s_rawMin  = 0;
static volatile int32_t  s_rawMax  = 0;
static volatile uint32_t s_readErr = 0;

// Which half of the I2S frame to capture. Switchable at runtime because the
// ESP32 receiver is known to disagree with the datasheets about which slot
// "left" is, and reflashing once per guess is not a debugging method.
// 0 = ONLY_LEFT, 1 = ONLY_RIGHT.
static volatile int s_chan    = MIC_DEFAULT_CHAN;
static volatile int s_chanReq = -1;

// How far to shift the 32-bit slot down to 16 bits. 16 is correct for a
// 24-bit left-justified sample; SPH0645-class parts need a different value.
static volatile int s_shift = MIC_DEFAULT_SHIFT;

// Battery monitoring
static volatile float s_batVoltage = 0.0f;  // volts
static volatile int   s_batPercent = 0;     // 0..100
static esp_adc_cal_characteristics_t s_adcChars;

static httpd_handle_t s_ui = nullptr, s_video = nullptr, s_sound = nullptr;

// WiFi provisioning
static const char* AP_SSID = "BabyCAM";
static const char* AP_PASSWORD = "12345678";
static const char* NVS_WIFI_NS = "wifi_creds";
static String g_sta_ssid = "";
static String g_sta_pass = "";
static bool g_provisioning = false;
static WebServer* g_prov_server = nullptr;

#if USE_NVS
// ----------------------------------------------------------------- NVS persistence
static Preferences prefs;

static void nvsLoad() {
  prefs.begin(NVS_NAMESPACE, true);  // read-only
  s_gain  = prefs.getInt("gain",  MIC_DEFAULT_GAIN);
  s_lamp  = prefs.getInt("lamp",  0);
  s_chan  = prefs.getInt("chan",  MIC_DEFAULT_CHAN);
  s_shift = prefs.getInt("shift", MIC_DEFAULT_SHIFT);
  prefs.end();
  // Clamp to valid ranges
  if (s_gain < 1) s_gain = 1; if (s_gain > 64) s_gain = 64;
  if (s_lamp < 0) s_lamp = 0; if (s_lamp > 255) s_lamp = 255;
  if (s_chan < 0) s_chan = 0; if (s_chan > 1) s_chan = 1;
  if (s_shift < 8) s_shift = 8; if (s_shift > 24) s_shift = 24;
}

static void nvsSaveGain(int g) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt("gain", g);
  prefs.end();
}

static void nvsSaveLamp(int v) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt("lamp", v);
  prefs.end();
}

static void nvsSaveChan(int c) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt("chan", c);
  prefs.end();
}

static void nvsSaveShift(int sh) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt("shift", sh);
  prefs.end();
}

// WiFi credentials NVS
static bool nvsLoadWiFiCreds(String& ssid, String& pass) {
  prefs.begin(NVS_WIFI_NS, true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

static void nvsSaveWiFiCreds(const String& ssid, const String& pass) {
  prefs.begin(NVS_WIFI_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

static void nvsClearWiFiCreds() {
  prefs.begin(NVS_WIFI_NS, false);
  prefs.clear();
  prefs.end();
}
#else
static void nvsLoad() {}
static void nvsSaveGain(int) {}
static void nvsSaveLamp(int) {}
static void nvsSaveChan(int) {}
static void nvsSaveShift(int) {}
static bool nvsLoadWiFiCreds(String&, String&) { return false; }
static void nvsSaveWiFiCreds(const String&, const String&) {}
static void nvsClearWiFiCreds() {}
#endif

// ----------------------------------------------------------------- WAV header
// The stream never ends, so the length fields are a lie either way. 0xFFFFFFFF
// is the obvious lie, but several browsers read it as -1 and refuse to start
// decoding while happily going on downloading -- a silent failure that looks
// exactly like a dead microphone. 0x7FFFFFFF is a lie they believe.
struct __attribute__((packed)) WavHeader {
  char     riff[4];
  uint32_t riffSize;
  char     wave[4];
  char     fmt[4];
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char     data[4];
  uint32_t dataSize;
};

static void buildWavHeader(WavHeader *h) {
  memcpy(h->riff, "RIFF", 4);
  h->riffSize = 0x7FFFFFFF;
  memcpy(h->wave, "WAVE", 4);
  memcpy(h->fmt, "fmt ", 4);
  h->fmtSize       = 16;
  h->audioFormat   = 1;             // PCM
  h->numChannels   = 1;
  h->sampleRate    = MIC_SAMPLE_RATE;
  h->bitsPerSample = 16;
  h->blockAlign    = h->numChannels * h->bitsPerSample / 8;
  h->byteRate      = h->sampleRate * h->blockAlign;
  memcpy(h->data, "data", 4);
  h->dataSize = 0x7FFFFFFF;
}

// ----------------------------------------------------------------- microphone
static bool micInit(int chan) {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = MIC_SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441 sends 24 in 32
  cfg.channel_format       = chan ? I2S_CHANNEL_FMT_ONLY_RIGHT
                                  : I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = MIC_BLOCK;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = false;
  cfg.fixed_mclk           = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = MIC_SCK_PIN;
  pins.ws_io_num    = MIC_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = MIC_SD_PIN;

  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) return false;
  s_chan = chan;
  return true;
}

// Always running, whether anyone listens or not: that keeps the level meter
// live and means a new listener hears sound immediately instead of waiting
// for the DMA chain to fill.
static void micTask(void *) {
  static int32_t raw[MIC_BLOCK];
  static int16_t pcm[MIC_BLOCK];

  for (;;) {
    // Re-install from inside this task: doing it from the HTTP handler would
    // tear the driver out from under a concurrent i2s_read.
    if (s_chanReq >= 0) {
      int want = s_chanReq; s_chanReq = -1;
      i2s_driver_uninstall(I2S_NUM_1);
      micInit(want);
      s_reads = 0; s_readErr = 0; s_rawMin = 0; s_rawMax = 0;
    }

    size_t got = 0;
    // Bounded wait: portMAX_DELAY would hide a peripheral that never delivers,
    // and the whole point here is to be able to see that case.
    esp_err_t err = i2s_read(I2S_NUM_1, raw, sizeof(raw), &got, pdMS_TO_TICKS(500));
    const int n = (err == ESP_OK) ? (int)(got / sizeof(int32_t)) : 0;
    if (err != ESP_OK) s_readErr++;
    if (n <= 0) {
        // Both failure paths MUST sleep. i2s_read can return instantly when the
        // peripheral delivers nothing, and a bare `continue` then spins at task
        // priority forever -- which starved the HTTP servers sharing this core
        // and made the whole board look hung while it still answered pings.
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
    }
    s_reads++;

    int32_t lo = raw[0], hi = raw[0];
    for (int i = 1; i < n; i++) {
        if (raw[i] < lo) lo = raw[i];
        if (raw[i] > hi) hi = raw[i];
    }
    s_rawMin = lo; s_rawMax = hi;

    const int gain = s_gain;
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) {
      // 24-bit sample sits left-justified in the slot; >>16 takes the top 16,
      // then software gain, because the INMP441 is quiet at conversational
      // distance and we want to hear breathing, not just crying.
      int32_t v = (raw[i] >> s_shift) * gain;
      if (v >  32767) v =  32767;
      if (v < -32768) v = -32768;
      pcm[i] = (int16_t)v;
      acc += (uint32_t)(v * v);
    }
    s_level = sqrtf((float)(acc / n)) / 32768.0f;

    // Never block: with no listener the buffer is full and samples are dropped
    // on purpose. Blocking here would stall the meter too.
    xStreamBufferSend(s_audio, pcm, n * sizeof(int16_t), 0);
  }
}

// ----------------------------------------------------------------- battery monitor
static void batTask(void *) {
  // Configure ADC
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten((adc1_channel_t)(BAT_ADC_PIN - 32), BAT_ADC_ATTEN); // GPIO33 = ADC1_CH5
  
  // Characterize ADC for voltage conversion
  esp_adc_cal_characterize(ADC_UNIT_1, BAT_ADC_ATTEN, ADC_WIDTH_BIT_12, 1100, &s_adcChars);
  
  for (;;) {
    uint32_t adcSum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
      adcSum += adc1_get_raw((adc1_channel_t)(BAT_ADC_PIN - 32));
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    uint32_t adcAvg = adcSum / BAT_SAMPLES;
    
    // Convert to voltage (mV) using calibration
    uint32_t voltage_mV = esp_adc_cal_raw_to_voltage(adcAvg, &s_adcChars);
    
    // Apply divider ratio
    float voltage = (voltage_mV / 1000.0f) * BAT_DIVIDER_RATIO;
    
    // Clamp and calculate percentage
    if (voltage > BAT_MAX_VOLTAGE) voltage = BAT_MAX_VOLTAGE;
    if (voltage < BAT_MIN_VOLTAGE) voltage = BAT_MIN_VOLTAGE;
    
    int percent = (int)((voltage - BAT_MIN_VOLTAGE) / (BAT_MAX_VOLTAGE - BAT_MIN_VOLTAGE) * 100.0f);
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    
    s_batVoltage = voltage;
    s_batPercent = percent;
    
    // Update every 30 seconds
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
}

// ----------------------------------------------------------------- lamp
// GPIO4 drives the bright white flash LED. Dimmable rather than on/off: at full
// power it is a camera flash, far too harsh to point at a cot.
// LEDC channel 4, because the camera driver holds channel 0 for the XCLK.
#define LAMP_PIN      4
#define LAMP_CHANNEL  4

static void lampInit() {
  ledcSetup(LAMP_CHANNEL, 5000, 8);
  ledcAttachPin(LAMP_PIN, LAMP_CHANNEL);
  ledcWrite(LAMP_CHANNEL, 0);
}

static void lampSet(int v) {
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  s_lamp = v;
  ledcWrite(LAMP_CHANNEL, v);
  nvsSaveLamp(v);
}

// ----------------------------------------------------------------- WiFi Provisioning HTML
static const char PROV_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BabyCAM Setup</title>
<style>
:root{--bg:#fff;--ink:#141414;--ink-soft:#6b6b6b;--line:#e4e4e4;--accent:#ff6200;--r:12px}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--ink:#f2f2f2;--ink-soft:#9a9a9a;--line:#2c2c2c}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:480px;margin:0 auto}
h1{font-size:18px;font-weight:600;margin:0 0 14px}
.card{border:1px solid var(--line);border-radius:var(--r);padding:14px;margin-bottom:14px}
.row{display:flex;align-items:center;gap:10px}
label{font-size:14px;color:var(--ink-soft);min-width:80px}
input[type=text],input[type=password],select{flex:1;padding:8px 10px;border:1px solid var(--line);border-radius:8px;font:inherit}
button{font:inherit;color:var(--ink);background:none;border:1px solid var(--line);border-radius:8px;padding:10px 16px;cursor:pointer;width:100%}
button.primary{border-color:var(--accent);color:var(--accent);font-weight:600}
button:disabled{opacity:0.5;cursor:not-allowed}
.muted{color:var(--ink-soft);font-size:13px}
.scan-btn{background:var(--accent);color:#fff;border-color:var(--accent)}
.networks{list-style:none;padding:0;margin:0;max-height:200px;overflow:auto;border:1px solid var(--line);border-radius:8px}
.networks li{padding:10px;border-bottom:1px solid var(--line);cursor:pointer}
.networks li:last-child{border-bottom:none}
.networks li:hover{background:var(--line)}
.networks li.selected{background:#fff3e0;border-color:var(--accent)}
.signal{display:inline-block;width:4px;height:12px;background:var(--accent);margin-right:8px;vertical-align:middle}
.signal.low{height:6px}.signal.med{height:9px}.signal.high{height:12px}
.hidden{display:none}
.status{padding:10px;border-radius:8px;margin-top:10px;text-align:center}
.status.ok{background:#e8f5e9;color:#2e7d32;border:1px solid #c8e6c9}
.status.err{background:#fdecea;color:#c62828;border:1px solid #f5c6cb}
</style></head><body><div class="wrap">
<h1>BabyCAM WiFi Setup</h1>
<div class="card">
  <p class="muted">Connect BabyCAM to your WiFi network. The device will remember these credentials.</p>
  <div class="networks" id="netlist"></div>
  <div class="row" style="margin-top:10px">
    <button class="scan-btn" id="scanBtn" onclick="scan()">Scan Networks</button>
  </div>
</div>
<div class="card hidden" id="credCard">
  <div class="row"><label>SSID</label><input type="text" id="ssid" readonly></div>
  <div class="row" style="margin-top:10px"><label>Password</label><input type="password" id="pass"></div>
  <button class="primary" id="connectBtn" onclick="connect()" style="margin-top:10px">Connect</button>
  <div class="status hidden" id="status"></div>
</div>
<script>
let selectedNet = null;
function scan(){
  const btn = document.getElementById('scanBtn');
  btn.disabled = true; btn.textContent = 'Scanning...';
  fetch('/scan').then(r=>r.json()).then(data=>{
    const list = document.getElementById('netlist');
    list.innerHTML = '';
    data.networks.forEach(n=>{
      const li = document.createElement('li');
      li.textContent = n.ssid + (n.sec ? ' 🔒' : ' 🔓');
      const sig = document.createElement('span');
      sig.className = 'signal ' + (n.rssi > -50 ? 'high' : n.rssi > -70 ? 'med' : 'low');
      li.prepend(sig);
      li.onclick = ()=>selectNet(li, n);
      list.appendChild(li);
    });
    btn.disabled = false; btn.textContent = 'Scan Again';
  }).catch(()=>{btn.disabled=false;btn.textContent='Scan Failed - Retry';});
}
function selectNet(el, net){
  document.querySelectorAll('#netlist li').forEach(l=>l.classList.remove('selected'));
  el.classList.add('selected');
  selectedNet = net;
  document.getElementById('ssid').value = net.ssid;
  document.getElementById('credCard').classList.remove('hidden');
  document.getElementById('pass').focus();
}
function connect(){
  if(!selectedNet) return;
  const btn = document.getElementById('connectBtn');
  const status = document.getElementById('status');
  btn.disabled = true; btn.textContent = 'Connecting...';
  status.className = 'status hidden';
  fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:selectedNet.ssid,pass:document.getElementById('pass').value})})
  .then(r=>r.json()).then(data=>{
    if(data.ok){
      status.className = 'status ok';
      status.textContent = 'Connected! BabyCAM will reboot...';
      setTimeout(()=>{window.location.href='http://'+location.hostname+'/';},3000);
    }else{
      status.className = 'status err';
      status.textContent = 'Failed: ' + (data.msg||'unknown error');
      btn.disabled = false; btn.textContent = 'Connect';
    }
  }).catch(()=>{
    status.className = 'status err';
    status.textContent = 'Request failed';
    btn.disabled = false; btn.textContent = 'Connect';
  });
}
scan();
</script>
</div></body></html>
)HTML";

// ----------------------------------------------------------------- provisioning handlers
static void provScanHandler() {
  if (!g_prov_server) return;
  int n = WiFi.scanComplete();
  if (n == -2) {
    WiFi.scanNetworks(true); // async
    g_prov_server->send(200, "application/json", "{\"networks\":[]}");
    return;
  }
  if (n == -1) {
    g_prov_server->send(200, "application/json", "{\"networks\":[]}");
    return;
  }
  String json = "{\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"sec\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]}";
  WiFi.scanDelete();
  WiFi.scanNetworks(true); // start next scan
  g_prov_server->send(200, "application/json", json);
}

static void provConnectHandler() {
  if (!g_prov_server) return;
  if (g_prov_server->method() != HTTP_POST) {
    g_prov_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  String body = g_prov_server->arg("plain");
  int ssidStart = body.indexOf("\"ssid\":\"") + 8;
  int ssidEnd = body.indexOf("\"", ssidStart);
  int passStart = body.indexOf("\"pass\":\"") + 8;
  int passEnd = body.indexOf("\"", passStart);
  if (ssidStart < 8 || ssidEnd < 0) {
    g_prov_server->send(400, "application/json", "{\"ok\":false,\"msg\":\"bad request\"}");
    return;
  }
  String ssid = body.substring(ssidStart, ssidEnd);
  String pass = (passStart >= 8 && passEnd >= 0) ? body.substring(passStart, passEnd) : "";
  
  // Try to connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    nvsSaveWiFiCreds(ssid, pass);
    g_prov_server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    g_prov_server->send(200, "application/json", "{\"ok\":false,\"msg\":\"connection failed\"}");
  }
}

static void provRootHandler() {
  if (g_prov_server) {
    g_prov_server->send_P(200, "text/html", PROV_HTML);
  }
}

static void startProvisioning() {
  Serial.println("[babycam] Starting provisioning AP...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[babycam] AP IP: %s\n", apIP.toString().c_str());
  Serial.printf("[babycam] Connect to '%s' / '%s' then browse http://%s/\n", AP_SSID, AP_PASSWORD, apIP.toString().c_str());

  g_prov_server = new WebServer(80);
  g_prov_server->on("/", provRootHandler);
  g_prov_server->on("/scan", provScanHandler);
  g_prov_server->on("/connect", provConnectHandler);
  g_prov_server->begin();
  WiFi.scanNetworks(true); // start async scan
  g_provisioning = true;
  
  // Run provisioning loop
  while (g_provisioning) {
    g_prov_server->handleClient();
    delay(10);
  }
  
  delete g_prov_server;
  g_prov_server = nullptr;
}

// ----------------------------------------------------------------- handlers
static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t levelHandler(httpd_req_t *req) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"level\":%.4f,\"gain\":%d,\"uptime\":%lu,\"rssi\":%d,\"heap\":%u,"
           "\"reads\":%lu,\"readErr\":%lu,\"rawMin\":%ld,\"rawMax\":%ld,\"lamp\":%d,\"chan\":%d,\"shift\":%d,"
           "\"batV\":%.2f,\"batPct\":%d}",
           s_level, s_gain, (unsigned long)(millis() / 1000),
           WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
           (unsigned long)s_reads, (unsigned long)s_readErr,
           (long)s_rawMin, (long)s_rawMax, s_lamp, s_chan, s_shift,
           s_batVoltage, s_batPercent);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setHandler(httpd_req_t *req) {
  char q[64], v[16];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "gain", v, sizeof(v)) == ESP_OK) {
    int g = atoi(v);
    if (g >= 1 && g <= 64) {
      s_gain = g;
      nvsSaveGain(g);
    }
  }
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "lamp", v, sizeof(v)) == ESP_OK) {
    lampSet(atoi(v));
  }
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "chan", v, sizeof(v)) == ESP_OK) {
    s_chanReq = atoi(v) ? 1 : 0;
    nvsSaveChan(s_chanReq);
  }
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "shift", v, sizeof(v)) == ESP_OK) {
    int sh = atoi(v);
    if (sh >= 8 && sh <= 24) {
      s_shift = sh;
      nvsSaveShift(sh);
    }
  }
  return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t audioHandler(httpd_req_t *req) {
  WavHeader h;
  buildWavHeader(&h);

  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (httpd_resp_send_chunk(req, (const char *)&h, sizeof(h)) != ESP_OK) return ESP_FAIL;

  // Start from silence rather than from whatever has been sitting in the ring.
  xStreamBufferReset(s_audio);

  uint8_t buf[1024];
  for (;;) {
    size_t n = xStreamBufferReceive(s_audio, buf, sizeof(buf), pdMS_TO_TICKS(300));
    if (n == 0) {
        // Send real silence rather than looping. A handler that never writes
        // never learns that the client has gone, and since this server runs
        // one request at a time it would hold the port shut for everybody.
        memset(buf, 0, 256);
        n = 256;
    }
    if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) break;
  }
  return ESP_OK;
}

#define BOUNDARY "frame"
static esp_err_t streamHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part[80];
  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    int len = snprintf(part, sizeof(part),
                       "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                       "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
    esp_err_t r = httpd_resp_send_chunk(req, part, len);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (r != ESP_OK) break;
  }
  return ESP_OK;
}

// ----------------------------------------------------------------- servers
// Three servers, not one. esp_http_server processes requests serially in a
// single task, so one endless handler would block every other endpoint.
// Each instance also needs its own ctrl_port - sharing it silently fails.
//
// lru_purge_enable is not optional here. Without it, once max_open_sockets is
// reached the server accepts further connections and closes them with no
// response at all -- every endpoint appears dead while the board still pings
// happily. A couple of forgotten browser tabs are enough to reach that state,
// because the page holds an audio stream and polls four times a second.
static void startServers() {
  httpd_uri_t uIndex = {"/",      HTTP_GET, indexHandler,  nullptr};
  httpd_uri_t uLevel = {"/level", HTTP_GET, levelHandler,  nullptr};
  httpd_uri_t uSet   = {"/set",   HTTP_GET, setHandler,    nullptr};
  httpd_uri_t uSnd   = {"/audio", HTTP_GET, audioHandler,  nullptr};
  httpd_uri_t uVid   = {"/stream",HTTP_GET, streamHandler, nullptr};

  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80; c.ctrl_port = 32768; c.max_uri_handlers = 4;
  c.lru_purge_enable = true; c.max_open_sockets = 7;
  if (httpd_start(&s_ui, &c) == ESP_OK) {
    httpd_register_uri_handler(s_ui, &uIndex);
    httpd_register_uri_handler(s_ui, &uLevel);
    httpd_register_uri_handler(s_ui, &uSet);
  }

  c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 81; c.ctrl_port = 32769;
  c.lru_purge_enable = true; c.max_open_sockets = 3;
  if (httpd_start(&s_video, &c) == ESP_OK) httpd_register_uri_handler(s_video, &uVid);

  c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 82; c.ctrl_port = 32770;
  c.lru_purge_enable = true; c.max_open_sockets = 3;
  if (httpd_start(&s_sound, &c) == ESP_OK) httpd_register_uri_handler(s_sound, &uSnd);
}

// ----------------------------------------------------------------- camera
static bool cameraInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;   c.pin_pclk  = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href  = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;   c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = CAM_FRAME_SIZE;
  c.jpeg_quality = CAM_JPEG_QUALITY;
  c.fb_count     = psramFound() ? 2 : 1;
  c.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  // Always hand out the newest frame; a queue of stale frames only adds lag.
  c.grab_mode    = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

// ----------------------------------------------------------------- setup
void setup() {
  // The 5 V regulator on these boards sags when the camera and WiFi start
  // together, and the brownout detector reboots a board that would otherwise
  // run fine. Standard practice on ESP32-CAM.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n[babycam] boot");

  nvsLoad();
  Serial.printf("[babycam] loaded: gain=%d lamp=%d chan=%d shift=%d\n",
                s_gain, s_lamp, s_chan, s_shift);

  lampInit();
  lampSet(s_lamp);  // restore lamp brightness

  if (!cameraInit()) {
    Serial.println("[babycam] camera init FAILED");
  } else {
    Serial.println("[babycam] camera ok");
  }

  s_audio = xStreamBufferCreate(AUDIO_RING_BYTES, sizeof(int16_t) * MIC_BLOCK);
  if (!micInit(s_chan)) Serial.println("[babycam] mic init FAILED");
  else                 Serial.println("[babycam] mic ok");

  // Microphone on core 1, away from the WiFi/LwIP stack on core 0.
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 5, nullptr, 1);

  // Battery monitor on core 1 (low priority)
  xTaskCreatePinnedToCore(batTask, "bat", 4096, nullptr, 1, nullptr, 1);

  // WiFi: try stored credentials first, otherwise start provisioning AP
  String ssid, pass;
  bool hasCreds = nvsLoadWiFiCreds(ssid, pass);
  
  if (hasCreds) {
    Serial.printf("[babycam] Found stored credentials for '%s', connecting...\n", ssid.c_str());
    WiFi.setHostname(HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    WiFi.setSleep(false);
    
    Serial.print("[babycam] wifi");
    uint8_t retry = 0;
    while (WiFi.status() != WL_CONNECTED) {
      delay(400);
      Serial.print('.');
      if (++retry >= 75) {  // ~30s
        Serial.println("\n[babycam] wifi timeout, starting provisioning AP...");
        break;
      }
      if (retry > 5) delay(1000 * (1 << min(retry - 5, 4)));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[babycam] http://%s/\n", WiFi.localIP().toString().c_str());
    } else {
      hasCreds = false; // Fall through to provisioning
    }
  }
  
  if (!hasCreds) {
    startProvisioning(); // This blocks until connected
    // After provisioning, we should have WiFi connected
    Serial.printf("\n[babycam] http://%s/\n", WiFi.localIP().toString().c_str());
  }

#if USE_MDNS
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[babycam] mDNS: http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("[babycam] mDNS start FAILED");
  }
#endif

  startServers();
}

void loop() {
  // Everything runs in tasks. Reconnect if the access point drops.
  static uint8_t wifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[babycam] wifi lost, reconnecting");
    WiFi.reconnect();
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 32s, 60s cap
    int r = wifiRetry < 6 ? wifiRetry : 5;
    uint32_t wait = 1000 * (1 << r);
    if (wifiRetry >= 6) wait = 60000;
    delay(wait);
    if (wifiRetry < 10) wifiRetry++;
  } else {
    wifiRetry = 0;
  }
  delay(1000);
}