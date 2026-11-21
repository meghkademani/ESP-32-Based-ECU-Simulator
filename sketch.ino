#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- Pins ----------
#define THROTTLE_PIN 34   // POT1  = Throttle pedal
#define TEMP_PIN     35   // POT2  = Coolant temperature sensor
#define BRAKE_PIN    25   // BUTTON = Brake pedal
#define LED_PIN      2    // LED   = Warning lamp
#define BUZZ_PIN     4    // BUZZER = Audible alarm

// ECU1 (Engine) -> Serial2: RX=16, TX=17
// ECU2 (Dash)   -> Serial1: RX=19, TX=18
HardwareSerial &engineLink = Serial2;
HardwareSerial &dashLink   = Serial1;

LiquidCrystal_I2C lcd(0x27, 16, 2);

uint8_t xorSum(const uint8_t *d, uint8_t n) {
  uint8_t c = 0;
  while (n--) c ^= *d++;
  return c;
}

// Turns the state into readable text for the Serial Monitor
String caseName(bool lost, uint8_t f) {
  if (lost) return "COMM LOST";
  String s = "";
  if (f & 2) s += "OVERHEAT ";
  if (f & 4) s += "OVER-REV ";
  if (f & 1) s += "BRAKING ";
  if (s == "") return "NORMAL";
  s.trim();
  return s;
}

// ================= ECU 1: ENGINE =================
// Frame: [0xAA][ID=0x01][LEN=5][RPM_H][RPM_L][SPEED][TEMP][FLAGS][XOR][0x55]
// FLAGS: bit0 = brake, bit1 = overheat, bit2 = over-rev
void engineTask(void *pv) {
  uint16_t rpm = 800;
  uint8_t speedKmh = 0;
  bool linkCut = false;
  int tick = 0;

  for (;;) {
    int throttle = map(analogRead(THROTTLE_PIN), 0, 4095, 0, 100);
    int tempC    = map(analogRead(TEMP_PIN), 0, 4095, 40, 130);
    bool brake   = !digitalRead(BRAKE_PIN);

    int target = 800 + throttle * 62;
    rpm += (target - (int)rpm) / 4;

    if (brake) {
      speedKmh = (speedKmh > 3) ? speedKmh - 3 : 0;
    } else {
      int targetSpeed = constrain((int)(rpm - 900) / 35, 0, 200);
      if (speedKmh < targetSpeed) speedKmh++;
      else if (speedKmh > targetSpeed) speedKmh--;
    }

    uint8_t flags = (brake ? 1 : 0) | (tempC > 105 ? 2 : 0) | (rpm > 6500 ? 4 : 0);

    uint8_t frame[10] = { 0xAA, 0x01, 5,
                          (uint8_t)(rpm >> 8), (uint8_t)rpm,
                          speedKmh, (uint8_t)tempC, flags, 0, 0x55 };
    frame[8] = xorSum(&frame[1], 7);

    // Type 'c' in the Serial Monitor to cut / restore the link
    if (Serial.available() && Serial.read() == 'c') {
      linkCut = !linkCut;
      Serial.println(linkCut ? "\n[ECU1] *** LINK CUT by user (no frames sent) ***\n"
                             : "\n[ECU1] *** LINK RESTORED by user ***\n");
    }
    if (!linkCut) engineLink.write(frame, 10);

    // Print engine status once per second (every 10 cycles)
    if (++tick % 10 == 0) {
      Serial.printf("[ECU1 TX] RPM=%u SPD=%u TEMP=%dC flags=%u%s\n",
                    rpm, speedKmh, tempC, flags, linkCut ? "  (LINK CUT)" : "");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ================= ECU 2: DASHBOARD =================
uint16_t d_rpm = 0;
uint8_t d_spd = 0, d_flags = 0;
int d_temp = 0;

bool parseByte(uint8_t b) {
  static uint8_t buf[10];
  static uint8_t i = 0;
  if (i == 0 && b != 0xAA) return false;
  buf[i++] = b;
  if (i == 10) {
    i = 0;
    if (buf[9] == 0x55 && buf[1] == 0x01 && buf[2] == 5 &&
        xorSum(&buf[1], 7) == buf[8]) {
      d_rpm = (buf[3] << 8) | buf[4];
      d_spd = buf[5];
      d_temp = buf[6];
      d_flags = buf[7];
      return true;
    }
  }
  return false;
}

void dashTask(void *pv) {
  uint32_t lastValid = 0, lcdT = 0, printT = 0;
  String lastCase = "";

  for (;;) {
    while (dashLink.available())
      if (parseByte(dashLink.read())) lastValid = millis();

    bool commLost = (millis() - lastValid) > 500;
    bool warn = commLost || (d_flags & 6);
    digitalWrite(LED_PIN, warn);

    // Buzzer: 2000 Hz beep, 250 ms on / 250 ms off while a warning is active
    static bool buzzOn = false;
    bool wantBuzz = warn && ((millis() / 250) % 2);
    if (wantBuzz && !buzzOn) { tone(BUZZ_PIN, 2000); buzzOn = true; }
    if (!wantBuzz && buzzOn) { noTone(BUZZ_PIN);     buzzOn = false; }

    // ---- Serial Monitor: exact case ----
    String cs = caseName(commLost, d_flags);
    if (cs != lastCase) {                       // print a banner when the case changes
      if (commLost)
        Serial.println("\n>>> [ECU2 DASH] CASE: COMM LOST  (no valid frame for > 500 ms) <<<\n");
      else
        Serial.printf("\n>>> [ECU2 DASH] CASE: %s  (RPM=%u SPD=%u TEMP=%dC) <<<\n\n",
                      cs.c_str(), d_rpm, d_spd, d_temp);
      lastCase = cs;
    }
    if (millis() - printT > 1000) {             // status line once per second
      printT = millis();
      if (commLost) Serial.println("[ECU2 RX] -- no data --   | CASE: COMM LOST");
      else Serial.printf("[ECU2 RX] RPM=%u SPD=%u TEMP=%dC | CASE: %s\n",
                         d_rpm, d_spd, d_temp, cs.c_str());
    }

    // ---- LCD (flicker-free) ----
    if (millis() - lcdT > 200) {
      lcdT = millis();
      if (commLost) {
        lcd.setCursor(0, 0); lcd.print("!! COMM LOST !! ");
        lcd.setCursor(0, 1); lcd.print("                ");
      } else {
        lcd.setCursor(0, 0); lcd.printf("RPM:%4u SPD:%3u", d_rpm, d_spd);
        lcd.setCursor(0, 1); lcd.printf("T:%3dC %-4s%-4s", d_temp,
                                        (d_flags & 2) ? "HOT" : "",
                                        (d_flags & 4) ? "REV" : "");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  engineLink.begin(115200, SERIAL_8N1, 16, 17);  // RX, TX
  dashLink.begin(115200, SERIAL_8N1, 19, 18);    // RX, TX

  pinMode(BRAKE_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);

  lcd.init();
  lcd.backlight();
  lcd.print("ECU Simulator");

  Serial.println("==============================================");
  Serial.println(" ESP32 Automotive ECU Communication Simulator");
  Serial.println("==============================================");
  Serial.println(" CASES SHOWN IN THIS TERMINAL:");
  Serial.println("  NORMAL     -> all values in range");
  Serial.println("  OVERHEAT   -> coolant temp > 105 C  (flag bit1)");
  Serial.println("  OVER-REV   -> RPM > 6500            (flag bit2)");
  Serial.println("  BRAKING    -> brake button held     (flag bit0)");
  Serial.println("  COMM LOST  -> no valid frame > 500 ms");
  Serial.println(" Type 'c' + Enter to cut / restore the UART link");
  Serial.println("==============================================\n");

  xTaskCreatePinnedToCore(engineTask, "ECU1_Engine", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(dashTask,   "ECU2_Dash",   6144, NULL, 1, NULL, 1);
}

void loop() { vTaskDelete(NULL); }
