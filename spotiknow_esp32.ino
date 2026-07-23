/*
  =====================================================================
  Spotiknow - ESP32 Firmware (single-file build) - v3
  =====================================================================
  Records 20 seconds of audio from an INMP441 I2S microphone and
  uploads it directly to the Spotiknow Hugging Face Space over HTTPS -
  one POST request per prediction, no WebSocket, no bridge server.
  The predicted genre is shown on a 16x2 I2C LCD. A 4x3 matrix keypad
  drives a simple menu; an active buzzer gives audio feedback.

  Changes from v2 (fixes a silent WiFi-timeout reboot loop, a chance
  of corrupted audio from unchecked socket writes, and confidently
  wrong predictions on near-silent recordings):
    - client.write() is no longer trusted to send the full chunk in
      one call. Every chunk is now written in a retry loop until all
      bytes are actually confirmed sent (or a hard timeout aborts the
      upload with a real error), so the byte count promised in
      Content-Length always matches what actually reaches the server.
      A partial/silent short-write here was the most likely source of
      corrupted audio even when the mic itself was capturing real
      sound.
    - The recording loop now yields briefly every chunk via a 1ms
      vTaskDelay, and connectWifi()/connectToSpaceWithRetry() print
      clear Serial progress so a stall shows up in the log instead of
      just going quiet for 20+ seconds before an unexplained reboot.
    - After capture, if the recorded peak amplitude never rises above
      a small noise floor, the upload is aborted before it ever
      reaches the server and the LCD shows "Mic too quiet" instead of
      silently sending near-zero audio and displaying a confident but
      meaningless genre guess. (This is what "reggae ~20%" actually
      was - the model's near-deterministic output on close-to-silent
      input, confirmed by uploading a silent clip directly on the HF
      Space UI.)
    - connectWifi() now retries indefinitely with clear Serial output
      instead of restarting the whole device on a 20s timeout, so a
      slow-to-associate router doesn't put the board in a boot loop.

  Required libraries (Arduino IDE -> Tools -> Manage Libraries):
    - LiquidCrystal I2C   (Frank de Brabander / Marco Schwartz)
    - Keypad              (Mark Stanley, Alexander Brevig)
    - ArduinoJson (v7+)   (Benoit Blanchon)
  Everything else (WiFi, WiFiClientSecure, driver/i2s.h, Wire) ships
  with the ESP32 Arduino core - no separate install needed.

  Pairs with: POST /predict_genre on the Hugging Face Space
  (see the accompanying app.py - both files must be updated together).
  =====================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// ============================= CONFIG ================================

// ---- Wi-Fi ------------------------------------------------------------
// IMPORTANT: double check these are your REAL network credentials in
// the copy you actually flash. A wrong/placeholder SSID here is a
// silent failure mode - the board just never joins the network.
// ESP32 only supports 2.4GHz networks, not 5GHz.
#define WIFI_SSID          "Yuvraj"
#define WIFI_PASSWORD      "qwerty1234"

// ---- Hugging Face Space ------------------------------------------------
// No "https://", no trailing slash. Double-check this against the
// actual URL of your Space in a browser - a typo here produces
// exactly the "connect failed" symptom.
#define HF_SPACE_HOST      "yuvraj-chopra-dl-gen-ai-project.hf.space"
#define HF_SPACE_PORT      443
#define HF_PREDICT_PATH    "/predict_genre"
#define HF_HEALTH_PATH     "/health"
// Must match the ESP32_API_KEY secret set on the Space. Leave "" if
// you haven't configured one.
#define HF_API_KEY         ""

// ---- Audio format - MUST match the model's expected input exactly ------
#define SAMPLE_RATE        16000
#define RECORD_SECONDS     20
#define CHANNELS           1
#define BITS_PER_SAMPLE    16
#define BYTES_PER_SAMPLE   (BITS_PER_SAMPLE / 8)
#define PCM_DATA_BYTES     ((uint32_t)SAMPLE_RATE * RECORD_SECONDS * BYTES_PER_SAMPLE * CHANNELS)
#define WAV_HEADER_BYTES   44
#define TOTAL_UPLOAD_BYTES (WAV_HEADER_BYTES + PCM_DATA_BYTES)

// Minimum peak amplitude (on a 16-bit signed scale, so max is 32767)
// a recording must reach before we trust it's real audio and not
// silence/noise floor. If the loudest sample in the whole recording
// never crosses this, we treat it as "mic didn't pick up anything"
// rather than upload it and show a misleadingly confident genre.
// Tune this down if you're recording quiet passages on purpose.
#define MIN_PEAK_AMPLITUDE  400

// ---- I2S microphone (INMP441) -------------------------------------------
#define I2S_PORT           I2S_NUM_0
#define I2S_SCK_PIN        18
#define I2S_WS_PIN         19
#define I2S_SD_PIN         23
// If MIN_PEAK_AMPLITUDE keeps tripping even with wiring double-checked
// and known-good audio playing right next to the mic, try flipping
// this to I2S_CHANNEL_FMT_ONLY_RIGHT - it depends on whether the
// INMP441's L/R pin is tied to GND (left) or VDD (right).
#define I2S_MIC_CHANNEL     I2S_CHANNEL_FMT_ONLY_LEFT
#define I2S_DMA_BUF_COUNT  16  // was 8 - more headroom before a slow client.write()
                                // over a mobile hotspot risks dropping samples
#define I2S_DMA_BUF_LEN    512

// ---- 16x2 I2C LCD --------------------------------------------------------
#define LCD_I2C_ADDR       0x27   // try 0x3F if the display stays blank
#define LCD_COLS           16
#define LCD_ROWS           2
#define LCD_SDA_PIN        21
#define LCD_SCL_PIN        22

// ---- Active buzzer (on/off only - no frequency control) ------------------
#define BUZZER_PIN         4

// ---- 4x3 matrix keypad ----------------------------------------------------
#define KEYPAD_ROWS        4
#define KEYPAD_COLS        3
static byte keypadRowPins[KEYPAD_ROWS] = {13, 12, 14, 27};
static byte keypadColPins[KEYPAD_COLS] = {26, 25, 33};
static char keypadKeys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// ---- Timeouts / retries --------------------------------------------------
#define WIFI_RETRY_LOG_INTERVAL_MS 5000   // how often to print a "still trying" line
#define HF_RESPONSE_TIMEOUT_MS   60000    // generous: tolerates a cold Space
#define CONNECT_MAX_ATTEMPTS     3
#define CONNECT_RETRY_DELAY_MS   1000
#define SOCKET_WRITE_TIMEOUT_MS  10000    // max time to fully flush one chunk

// ============================ GLOBALS ==================================

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
Keypad keypad = Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

enum AppState {
  STATE_MENU,
  STATE_RESULT,
  STATE_LAST_PREDICTION,
  STATE_SYSTEM_INFO,
  STATE_ERROR
};

static AppState appState = STATE_MENU;
static bool     hasLastPrediction = false;
static String   lastGenre = "";
static float    lastConfidence = 0.0f;

struct PredictionResult {
  bool   success;
  String genre;
  float  confidence;
  String errorMessage;
};

// ============================ LCD HELPERS ================================

String truncate16(String s) {
  if (s.length() > LCD_COLS) s = s.substring(0, LCD_COLS);
  return s;
}

// No default argument here on purpose - Arduino's auto-generated function
// prototypes can conflict with default arguments on some IDE versions.
// Every call site below passes both lines explicitly.
void lcdShow(const String& line1, const String& line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(truncate16(line1));
  lcd.setCursor(0, 1);
  lcd.print(truncate16(line2));
}

// ============================ BUZZER HELPERS ==============================

static void beep(int onMs, int offMs, int repeats) {
  for (int i = 0; i < repeats; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < repeats - 1) delay(offMs);
  }
}

void beepStartup()     { beep(80, 80, 2); }
void beepMenuSelect()  { beep(30, 0, 1); }
void beepInvalid()     { beep(40, 60, 2); }
void beepRecordStart() { beep(150, 0, 1); }
void beepRecordStop()  { beep(60, 60, 2); }
void beepSuccess()     { beep(200, 0, 1); }
void beepError()       { beep(80, 80, 3); }

// ============================ WAV HEADER ==================================

// Writes a 44-byte canonical PCM WAV header describing `dataBytes` of
// audio at SAMPLE_RATE / CHANNELS / BITS_PER_SAMPLE. Bytes are written
// explicitly in little-endian order (not via memcpy of a uint32_t) so
// this is correct regardless of host byte order.
void buildWavHeader(uint8_t* header, uint32_t dataBytes) {
  uint32_t chunkSize  = 36 + dataBytes;
  uint32_t byteRate   = (uint32_t)SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE;
  uint16_t blockAlign = CHANNELS * BYTES_PER_SAMPLE;

  memcpy(header + 0, "RIFF", 4);
  header[4]  = chunkSize & 0xFF;         header[5]  = (chunkSize >> 8) & 0xFF;
  header[6]  = (chunkSize >> 16) & 0xFF; header[7]  = (chunkSize >> 24) & 0xFF;
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;  // Subchunk1Size = 16 (PCM)
  header[20] = 1;  header[21] = 0;                                   // AudioFormat = 1 (PCM)
  header[22] = CHANNELS & 0xFF; header[23] = 0;
  header[24] = SAMPLE_RATE & 0xFF;         header[25] = (SAMPLE_RATE >> 8) & 0xFF;
  header[26] = (SAMPLE_RATE >> 16) & 0xFF; header[27] = (SAMPLE_RATE >> 24) & 0xFF;
  header[28] = byteRate & 0xFF;         header[29] = (byteRate >> 8) & 0xFF;
  header[30] = (byteRate >> 16) & 0xFF; header[31] = (byteRate >> 24) & 0xFF;
  header[32] = blockAlign & 0xFF; header[33] = (blockAlign >> 8) & 0xFF;
  header[34] = BITS_PER_SAMPLE & 0xFF; header[35] = 0;
  memcpy(header + 36, "data", 4);
  header[40] = dataBytes & 0xFF;         header[41] = (dataBytes >> 8) & 0xFF;
  header[42] = (dataBytes >> 16) & 0xFF; header[43] = (dataBytes >> 24) & 0xFF;
}

// ============================ I2S SETUP ====================================

void audioInit() {
  i2s_config_t i2sConfig = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_MIC_CHANNEL,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = I2S_DMA_BUF_COUNT,
    .dma_buf_len          = I2S_DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_driver_install failed: %d\n", err);
    lcdShow("I2S init failed", "check wiring");
    while (true) delay(1000);
  }

  i2s_pin_config_t pinConfig = {
    .bck_io_num   = I2S_SCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD_PIN
  };
  i2s_set_pin(I2S_PORT, &pinConfig);
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[AUDIO] I2S ready.");
}

// ============================ CONNECTION HELPER ========================

// Attempts to open a TLS connection to the Space, retrying up to
// `maxAttempts` times with a short delay between tries, updating the
// LCD and Serial log on every attempt. Returns true on success. This
// is used for both the boot-time health check and the real upload,
// so a slow/cold Space or a transient handshake failure doesn't
// immediately dead-end the user with no explanation.
bool connectToSpaceWithRetry(WiFiClientSecure& client, int maxAttempts, const String& progressLabel) {
  char errBuf[128];

  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    lcdShow(progressLabel, "try " + String(attempt) + "/" + String(maxAttempts));
    Serial.printf("[HTTP] %s - connect attempt %d/%d - free heap %u bytes\n",
                  progressLabel.c_str(), attempt, maxAttempts, ESP.getFreeHeap());

    // DNS check, logged every attempt: tells us whether the host
    // resolves to an IP at all, before blaming the TLS handshake.
    IPAddress resolvedIP;
    if (WiFi.hostByName(HF_SPACE_HOST, resolvedIP)) {
      Serial.printf("[DNS] %s -> %s\n", HF_SPACE_HOST, resolvedIP.toString().c_str());
    } else {
      Serial.println("[DNS] resolution FAILED");
    }

    if (client.connect(HF_SPACE_HOST, HF_SPACE_PORT)) {
      Serial.println("[HTTP] connected.");
      return true;
    }

    errBuf[0] = '\0';
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[HTTP] connect failed: %s\n", errBuf[0] ? errBuf : "(no error detail returned)");
    client.stop();

    if (attempt < maxAttempts) delay(CONNECT_RETRY_DELAY_MS);
  }
  return false;
}

// Writes `len` bytes to `client`, retrying until every byte is
// actually accepted by the socket or `timeoutMs` elapses with no
// forward progress. client.write() is allowed to write fewer bytes
// than requested (e.g. when the TLS send buffer is momentarily full);
// the old code ignored that possibility and just assumed the whole
// chunk went out, which could silently desync the byte count from
// what was promised in Content-Length and corrupt the uploaded WAV.
// Returns true only if all `len` bytes were confirmed written.
bool writeAllWithTimeout(WiFiClientSecure& client, const uint8_t* buf, size_t len, unsigned long timeoutMs) {
  size_t sent = 0;
  unsigned long start = millis();
  while (sent < len) {
    if (!client.connected()) {
      Serial.println("[HTTP] socket dropped mid-write");
      return false;
    }
    size_t n = client.write(buf + sent, len - sent);
    if (n > 0) {
      sent += n;
      start = millis(); // progress made, reset the stall timer
    } else {
      if (millis() - start > timeoutMs) {
        Serial.printf("[HTTP] write stalled: %u/%u bytes sent\n", (unsigned)sent, (unsigned)len);
        return false;
      }
      delay(2);
    }
  }
  return true;
}

// ============================ HTTP HELPERS ===================================

// Reads one line (up to "\n", "\r" stripped) from `client`, giving up
// after `timeoutMs` of no new data. Returns "" both for a genuine
// blank line (end of headers) and for a timeout - callers that need
// to tell those apart check client.connected() separately.
String readLineWithTimeout(WiFiClientSecure& client, unsigned long timeoutMs) {
  String line;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') return line;
      if (c != '\r') line += c;
      start = millis();
    }
    if (!client.connected() && !client.available()) break;
    delay(2);
  }
  return line;
}

// Blocks until `len` bytes have been read into `buf`, or `timeoutMs`
// elapses with no new data. Returns the number of bytes actually read.
size_t readExactWithTimeout(WiFiClientSecure& client, uint8_t* buf, size_t len, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < len && millis() - start < timeoutMs) {
    if (client.available()) {
      int n = client.read(buf + got, len - got);
      if (n > 0) { got += n; start = millis(); }
    } else {
      if (!client.connected()) break;
      delay(2);
    }
  }
  return got;
}

// ============================ CORE: RECORD + UPLOAD + PREDICT =================

PredictionResult recordAndPredict() {
  PredictionResult result = { false, "", 0.0f, "" };

  // static: allocated once in .bss, not on the stack, and reused across
  // calls - avoids both stack overflow risk and malloc/free heap churn.
  static const int SEND_CHUNK_SAMPLES = 512;
  static const int SEND_CHUNK_BYTES   = SEND_CHUNK_SAMPLES * BYTES_PER_SAMPLE;
  static int32_t rawSamples[SEND_CHUNK_SAMPLES];
  static uint8_t pcmChunk[SEND_CHUNK_BYTES];
  static const size_t RESPONSE_BODY_MAX = 4096;
  static uint8_t bodyBuf[RESPONSE_BODY_MAX];

  // Immediate feedback the instant '1' is pressed - the user sees a
  // message before any network activity even starts.
  lcdShow("Connecting...", "please wait");

  WiFiClientSecure client;
  client.setInsecure(); // skips CA validation to keep firmware simple -
                         // see the implementation guide for CA pinning

  if (!connectToSpaceWithRetry(client, CONNECT_MAX_ATTEMPTS, "Connecting...")) {
    result.errorMessage = "connect failed";
    return result;
  }

  // ---- Request headers ---------------------------------------------------
  client.print(String("POST ") + HF_PREDICT_PATH + " HTTP/1.1\r\n");
  client.print(String("Host: ") + HF_SPACE_HOST + "\r\n");
  client.print("Content-Type: audio/wav\r\n");
  client.print(String("Content-Length: ") + String((uint32_t)TOTAL_UPLOAD_BYTES) + "\r\n");
  if (strlen(HF_API_KEY) > 0) {
    client.print(String("X-API-Key: ") + HF_API_KEY + "\r\n");
  }
  client.print("Connection: close\r\n");
  client.print("\r\n");

  // ---- WAV header ----------------------------------------------------------
  uint8_t wavHeader[WAV_HEADER_BYTES];
  buildWavHeader(wavHeader, PCM_DATA_BYTES);
  if (!writeAllWithTimeout(client, wavHeader, WAV_HEADER_BYTES, SOCKET_WRITE_TIMEOUT_MS)) {
    client.stop();
    result.errorMessage = "upload failed (header)";
    return result;
  }

  // ---- Stream PCM audio live from I2S, writing straight to the socket -------
  beepRecordStart();
  uint32_t bytesSent = 0;
  unsigned long lastLcdUpdate = 0;
  bool aborted = false;
  bool stoppedEarly = false;
  bool writeFailed = false;

  lcdShow("Recording 20s", "#stop  *cancel");
  delay(1200); // let the hint be readable before the stopwatch takes over

  // Tracks the loudest samples seen this recording. Used after capture
  // to detect a silent/near-silent recording before we ever upload it.
  int16_t sampleMin = 32767;
  int16_t sampleMax = -32768;

  while (bytesSent < PCM_DATA_BYTES) {
    char key = keypad.getKey();
    if (key == '*') { aborted = true; break; }
    if (key == '#') { stoppedEarly = true; break; }

    size_t bytesRead = 0;
    i2s_read(I2S_PORT, rawSamples, sizeof(rawSamples), &bytesRead, portMAX_DELAY);
    int samplesRead = bytesRead / sizeof(int32_t);

    for (int i = 0; i < samplesRead; i++) {
      // INMP441 samples are 24-bit, left-justified in a 32-bit word.
      // Shifting right 16 keeps the most significant 16 bits - a
      // clean, low-noise 16-bit PCM value.
      int16_t sample = (int16_t)(rawSamples[i] >> 16);
      if (sample < sampleMin) sampleMin = sample;
      if (sample > sampleMax) sampleMax = sample;
      pcmChunk[i * 2 + 0] = (uint8_t)(sample & 0xFF);
      pcmChunk[i * 2 + 1] = (uint8_t)((sample >> 8) & 0xFF);
    }

    size_t chunkBytes = samplesRead * BYTES_PER_SAMPLE;
    if (bytesSent + chunkBytes > PCM_DATA_BYTES) chunkBytes = PCM_DATA_BYTES - bytesSent;

    if (!writeAllWithTimeout(client, pcmChunk, chunkBytes, SOCKET_WRITE_TIMEOUT_MS)) {
      writeFailed = true;
      break;
    }
    bytesSent += chunkBytes;

    // Live stopwatch, once a second, built into fixed char buffers
    // (no per-update String concatenation, which fragments the heap).
    if (millis() - lastLcdUpdate > 1000) {
      int elapsed = (int)((bytesSent * RECORD_SECONDS) / PCM_DATA_BYTES);
      int barSegments = map(elapsed, 0, RECORD_SECONDS, 0, 10);

      char bar[13];
      bar[0] = '[';
      for (int i = 0; i < 10; i++) bar[i + 1] = (i < barSegments) ? '#' : '.';
      bar[11] = ']';
      bar[12] = '\0';

      char line1[17];
      snprintf(line1, sizeof(line1), "%ds/%ds  #stop", elapsed, RECORD_SECONDS);

      lcdShow(String(line1), String(bar));
      lastLcdUpdate = millis();
    }

    // Yield briefly every chunk (~32ms of audio per chunk at 16kHz) so
    // background WiFi/system tasks always get scheduled during a long
    // recording, instead of a tight loop of I2S reads + TLS writes
    // potentially starving them for the entire 20s.
    vTaskDelay(1);
  }

  if (aborted) {
    client.stop();
    beepRecordStop();
    result.errorMessage = "cancelled";
    return result;
  }

  if (writeFailed) {
    client.stop();
    beepError();
    result.errorMessage = "upload failed (dropped)";
    return result;
  }

  if (stoppedEarly && bytesSent < PCM_DATA_BYTES) {
    // Zero-pad the remainder so the server still receives exactly
    // TOTAL_UPLOAD_BYTES. The model already treats a silence-padded
    // short clip as valid (see pad_or_crop in app.py).
    static uint8_t zeros[512] = {0};
    uint32_t remaining = PCM_DATA_BYTES - bytesSent;
    while (remaining > 0) {
      size_t n = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
      if (!writeAllWithTimeout(client, zeros, n, SOCKET_WRITE_TIMEOUT_MS)) {
        client.stop();
        result.errorMessage = "upload failed (pad)";
        return result;
      }
      remaining -= n;
    }
    beepRecordStop();
  }

  Serial.printf("[AUDIO] captured sample range: min=%d max=%d\n", sampleMin, sampleMax);

  // If the loudest sample in the whole recording never rose above the
  // noise floor, this was silence (or the mic isn't wired/reading
  // correctly) - don't upload it and don't show a misleadingly
  // confident genre for it. This is what "reggae ~20%" actually was.
  int16_t peakAmplitude = max((int)sampleMax, (int)(-(int)sampleMin));
  if (peakAmplitude < MIN_PEAK_AMPLITUDE) {
    client.stop();
    Serial.printf("[AUDIO] peak %d below threshold %d - treating as silence\n", peakAmplitude, MIN_PEAK_AMPLITUDE);
    result.errorMessage = "mic too quiet";
    return result;
  }

  lcdShow("Analyzing...", "up to 60s (cold)");

  // ---- Read response ------------------------------------------------------
  String statusLine = readLineWithTimeout(client, HF_RESPONSE_TIMEOUT_MS);
  Serial.printf("[HTTP] Status line: %s\n", statusLine.c_str());

  int statusCode = 0;
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace > 0) statusCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();

  long contentLength = -1;
  while (true) {
    String headerLine = readLineWithTimeout(client, HF_RESPONSE_TIMEOUT_MS);
    if (headerLine.length() == 0) break; // blank line = end of headers
    String lower = headerLine;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = headerLine.substring(headerLine.indexOf(':') + 1).toInt();
    }
  }

  if (contentLength <= 0 || (size_t)contentLength >= RESPONSE_BODY_MAX) {
    client.stop();
    result.errorMessage = "bad response";
    return result;
  }

  size_t bodyLen = readExactWithTimeout(client, bodyBuf, (size_t)contentLength, HF_RESPONSE_TIMEOUT_MS);
  bodyBuf[bodyLen] = '\0';
  client.stop();

  if (statusCode != 200) {
    Serial.printf("[HTTP] Error body: %s\n", (char*)bodyBuf);
    result.errorMessage = "server error " + String(statusCode);
    return result;
  }

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, (char*)bodyBuf, bodyLen);
  if (jsonErr) {
    result.errorMessage = "bad json";
    return result;
  }

  result.success    = true;
  result.genre      = doc["genre"].as<String>();
  result.confidence = doc["confidence"] | 0.0f;
  return result;
}

// ============================ WIFI / SPACE CHECKS =============================

void connectWifi() {
  lcdShow("WiFi connecting", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  unsigned long lastLog = 0;
  // Retries indefinitely with visible progress instead of restarting
  // the whole board on a timeout - a restart-on-timeout loop is
  // indistinguishable, from the outside, from a hard crash, and hides
  // the real cause (wrong SSID/password, router out of range, etc.)
  // instead of surfacing it.
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    if (millis() - lastLog > WIFI_RETRY_LOG_INTERVAL_MS) {
      Serial.printf("[WIFI] still connecting to \"%s\"... (status=%d) - check SSID/password/2.4GHz if this repeats\n",
                    WIFI_SSID, WiFi.status());
      lcdShow("WiFi connecting", String((millis() - start) / 1000) + "s elapsed");
      lastLog = millis();
    }
  }
  Serial.printf("[WIFI] connected, IP: %s\n", WiFi.localIP().toString().c_str());
  lcdShow("WiFi connected", WiFi.localIP().toString());
  delay(1000);
}

void checkSpaceReachable() {
  WiFiClientSecure client;
  client.setInsecure();

  if (connectToSpaceWithRetry(client, 2, "Checking HF")) {
    client.print(String("GET ") + HF_HEALTH_PATH + " HTTP/1.1\r\n");
    client.print(String("Host: ") + HF_SPACE_HOST + "\r\n");
    client.print("Connection: close\r\n\r\n");

    unsigned long start = millis();
    while (client.connected() && millis() - start < HF_RESPONSE_TIMEOUT_MS) {
      if (client.available()) client.read();
      else if (!client.connected()) break;
    }
    client.stop();
    lcdShow("HF Space online", "Ready.");
  } else {
    lcdShow("HF Space", "unreachable");
    beepError();
  }
  delay(1200);
}

// ============================ MENU / STATE SCREENS =============================

void showMenu() {
  lcdShow("1:Record 2:Last", "3:Info");
}

void showLastPrediction() {
  if (!hasLastPrediction) {
    lcdShow("Last Prediction", "None yet");
  } else {
    String upper = lastGenre; upper.toUpperCase();
    lcdShow("GENRE: " + upper, "Confidence: " + String((int)lastConfidence) + "%");
  }
}

// Cycles IP / heap / uptime every 2 seconds. Pass forceRedraw=true the
// moment this screen is entered so it draws immediately instead of
// waiting for the first 2-second tick; pass false on every subsequent
// poll from loop() so it only redraws (and clears) when the page
// actually changes - calling lcd.clear() every loop iteration would
// flicker the display continuously.
void showSystemInfo(bool forceRedraw) {
  static int page = 0;
  static unsigned long lastSwitch = 0;
  bool pageChanged = false;

  if (forceRedraw) {
    page = 0;
    lastSwitch = millis();
    pageChanged = true;
  } else if (millis() - lastSwitch > 2000) {
    page = (page + 1) % 3;
    lastSwitch = millis();
    pageChanged = true;
  }

  if (!pageChanged) return;

  if (page == 0)      lcdShow("IP:" + WiFi.localIP().toString(), "any key: menu");
  else if (page == 1) lcdShow("Heap: " + String(ESP.getFreeHeap() / 1024) + " KB", "any key: menu");
  else                lcdShow("Uptime: " + String(millis() / 1000) + "s", "any key: menu");
}

// ============================ SETUP / LOOP =======================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[BOOT] spotiknow starting...");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcdShow("SPOTIKNOW", "Booting...");

  keypad.setDebounceTime(15);
  audioInit();

  connectWifi();
  checkSpaceReachable();

  beepStartup();
  appState = STATE_MENU;
  showMenu();
}

void loop() {
  char key = keypad.getKey();

  switch (appState) {

    case STATE_MENU: {
      if (key == '1') {
        beepMenuSelect();
        PredictionResult result = recordAndPredict();

        if (!result.success && result.errorMessage == "cancelled") {
          appState = STATE_MENU;
          showMenu();
        } else if (!result.success) {
          beepError();
          lcdShow("ERROR", truncate16(result.errorMessage));
          appState = STATE_ERROR;
        } else {
          beepSuccess();
          hasLastPrediction = true;
          lastGenre = result.genre;
          lastConfidence = result.confidence;
          String upper = lastGenre; upper.toUpperCase();
          lcdShow("GENRE: " + upper, "Confidence: " + String((int)lastConfidence) + "%");
          appState = STATE_RESULT;
        }
      }
      else if (key == '2') {
        beepMenuSelect();
        appState = STATE_LAST_PREDICTION;
        showLastPrediction();
      }
      else if (key == '3') {
        beepMenuSelect();
        appState = STATE_SYSTEM_INFO;
        showSystemInfo(true);
      }
      else if (key != NO_KEY) {
        beepInvalid();
        lcdShow("Invalid key", "Try 1, 2 or 3");
        delay(700);
        showMenu();
      }
      break;
    }

    case STATE_SYSTEM_INFO: {
      showSystemInfo(false); // no-ops unless the 2s page timer has rolled over
      if (key != NO_KEY) {
        appState = STATE_MENU;
        showMenu();
      }
      break;
    }

    case STATE_RESULT:
    case STATE_LAST_PREDICTION:
    case STATE_ERROR: {
      if (key != NO_KEY) {
        appState = STATE_MENU;
        showMenu();
      }
      break;
    }
  }

  delay(20);
}
