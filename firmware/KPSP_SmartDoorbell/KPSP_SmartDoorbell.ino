#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiManager.h>        // Library: WiFiManager by tzapu
#include "esp_http_server.h"    // Espressif HTTP Server
#include "soc/soc.h"            // Used to disable brownout detector
#include "soc/rtc_cntl_reg.h"
#include <lwip/sockets.h>
#include <driver/i2s.h>         // Required for Microphone

// ==========================================
// PIN DEFINITIONS 
// ==========================================
#define PIR_PIN         14
#define SOLENOID_PIN    13
#define BUZZER_LED_PIN  4

// Microphone Pins (From Schematic)
#define MIC_I2S_SD      2
#define MIC_I2S_WS      15
#define MIC_I2S_SCK     12

// ESP32-CAM Camera Pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

httpd_handle_t stream_httpd = NULL;

// ==========================================
// 1. VIDEO STREAM HANDLER (MJPEG)
// ==========================================
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[128]; 

  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd < 0) return ESP_FAIL;

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
  if (res != ESP_OK) return res;

  while (true) {
    char temp_buf;
    if (recv(sockfd, &temp_buf, 1, MSG_PEEK | MSG_DONTWAIT) == 0) break; 

    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) break;
    vTaskDelay(40 / portTICK_PERIOD_MS); 
  }
  return res;
}

// ==========================================
// 2. STILL CAPTURE HANDLER (CiRA Core Face Scan)
// ==========================================
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;

  fb = esp_camera_fb_get();
  if (fb) {
    esp_camera_fb_return(fb);
    fb = NULL;
  }

  fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ==========================================
// 3. AUDIO STREAM HANDLER
// ==========================================
const char wav_header[44] = {
  'R', 'I', 'F', 'F', 
  0xFF, 0xFF, 0xFF, 0xFF, // Fake infinite size
  'W', 'A', 'V', 'E', 
  'f', 'm', 't', ' ', 
  16, 0, 0, 0,            
  1, 0,                   
  1, 0,                   
  0x80, 0x3E, 0x00, 0x00, // SampleRate (16000 Hz)
  0x00, 0x7D, 0x00, 0x00, // ByteRate
  2, 0,                   
  16, 0,                  // BitsPerSample (16 bits)
  'd', 'a', 't', 'a', 
  0xFF, 0xFF, 0xFF, 0xFF  
};

static esp_err_t audio_handler(httpd_req_t *req) {
  esp_err_t res = ESP_OK;
  size_t bytes_read;
  uint8_t i2s_data[1024]; 

  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd < 0) return ESP_FAIL;

  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  res = httpd_resp_send_chunk(req, wav_header, 44);
  if (res != ESP_OK) return res;

  Serial.println("Client connected to audio stream.");

  while (true) {
    char temp_buf;
    if (recv(sockfd, &temp_buf, 1, MSG_PEEK | MSG_DONTWAIT) == 0) break;

    i2s_read(I2S_NUM_1, &i2s_data, sizeof(i2s_data), &bytes_read, portMAX_DELAY);
    
    if (bytes_read > 0) {
      res = httpd_resp_send_chunk(req, (const char*)i2s_data, bytes_read);
      if (res != ESP_OK) break; 
    }
  }
  
  Serial.println("Audio stream ended safely.");
  return res;
}

// ==========================================
// 4. KODULAR APP ENDPOINTS (NEW / RESTORED)
// ==========================================

// A. Remote Unlock Handler (Triggered by pressing "Unlock" in the App)
static esp_err_t open_handler(httpd_req_t *req) {
  Serial.println("[App Command] OPEN Door Lock request received.");
  
  // Play a quick success chirp on the buzzer
  tone(BUZZER_LED_PIN, 2000, 100);
  delay(120);
  tone(BUZZER_LED_PIN, 2500, 150);

  // Pulse Solenoid open for 3 seconds
  digitalWrite(SOLENOID_PIN, HIGH);
  delay(3000); 
  digitalWrite(SOLENOID_PIN, LOW);
  
  // Send confirmation back to Kodular
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, "Door Opened", -1); // Matches "Door Opened" block check
  return ESP_OK;
}

// B. Status Check Handler (Polled by Kodular Clock timer to detect motion)
static esp_err_t status_handler(httpd_req_t *req) {
  int motion = digitalRead(PIR_PIN);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  if (motion == HIGH) {
    httpd_resp_send(req, "1", -1); // Motion active -> App shows Alert & triggers push notice
  } else {
    httpd_resp_send(req, "0", -1); // No motion -> App goes back to quiet state
  }
  return ESP_OK;
}

// ==========================================
// INITIALIZATION FUNCTIONS
// ==========================================
void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA; 
    config.jpeg_quality = 12;
    config.fb_count = 1;
  } else {
    config.frame_size = FRAMESIZE_QVGA; 
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_camera_init(&config);
}

void setupMicrophone() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = MIC_I2S_SCK,
    .ws_io_num = MIC_I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_I2S_SD
  };

  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pin_config);
}

void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80; 

  httpd_uri_t stream_uri =  { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture.jpg", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t audio_uri =   { .uri = "/audio", .method = HTTP_GET, .handler = audio_handler, .user_ctx = NULL };
  httpd_uri_t open_uri =    { .uri = "/open", .method = HTTP_GET, .handler = open_handler, .user_ctx = NULL };
  httpd_uri_t status_uri =  { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &capture_uri);
    httpd_register_uri_handler(stream_httpd, &audio_uri); 
    httpd_register_uri_handler(stream_httpd, &open_uri);   
    httpd_register_uri_handler(stream_httpd, &status_uri); 
    
    Serial.println("Servers running at:");
    Serial.println("Video:  http://" + WiFi.localIP().toString() + "/stream");
    Serial.println("Still:  http://" + WiFi.localIP().toString() + "/capture.jpg");
    Serial.println("Audio:  http://" + WiFi.localIP().toString() + "/audio");
    Serial.println("Status: http://" + WiFi.localIP().toString() + "/status");
    Serial.println("Open:   http://" + WiFi.localIP().toString() + "/open");
  }
}

// ==========================================
// MAIN SETUP & LOOP
// ==========================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  
  Serial.begin(115200);
  delay(1000);

  pinMode(PIR_PIN, INPUT);
  pinMode(SOLENOID_PIN, OUTPUT);
  pinMode(BUZZER_LED_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);
  digitalWrite(BUZZER_LED_PIN, LOW);

  setupCamera();
  setupMicrophone();

  WiFiManager wm;
  Serial.println("\nChecking Wi-Fi configuration...");
  if (!wm.autoConnect("SmartDoorbell_AP", "doorbell123")) {
    Serial.println("Failed to connect or timed out. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Connected to Wi-Fi successfully!");
  startServers();
}

void loop() {
  // Asynchronous background web server operates independently.
  // loop() handles physical visitor doorbell pushes / PIR triggers.
  if (digitalRead(PIR_PIN) == HIGH) {
    Serial.println("[PIR Event] Motion detected near door! Chiming...");
    
    // Play a polite, real "Ding-Dong" chime instead of a continuous screeching buzz
    tone(BUZZER_LED_PIN, 660, 400); // Play Note E5
    delay(450);
    tone(BUZZER_LED_PIN, 550, 600); // Play Note C#5
    
    // 5-second cooldown to avoid continuous double-triggers
    delay(5000); 
  }
  delay(100);
}