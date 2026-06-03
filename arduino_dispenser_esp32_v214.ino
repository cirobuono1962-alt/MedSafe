/*
 * ============================================================
 *  MedDispenser ESP32 v100.0
 *  Dispenser Automatico Medicine  Famiglia Buono
 * ============================================================
 *
 *  PROPRIETARIO: Famiglia Buono  Castellammare di Stabia
 *  SVILUPPATORE: Ciro Buono
 *
 *  CREDENZIALI DI SISTEMA:
 *  WiFi SSID:        FASTWEB-87En39
 *  WiFi Password:    bmJQGp2PUZ
 *  Firebase DB:      dispenser-famiglia-buono-4cd30
 *  Telegram Bot:     @mio_dispenser_bot
 *  Telegram Ciro:    Chat ID 5097242921
 *  Telegram Assunta: Chat ID 8787926485
 *  CallMeBot phone:  +393925249858
 *  CallMeBot apikey: 7046104
 *  IP ESP32:         192.168.1.122
 *  Web log:          http://192.168.1.122
 *  App PWA:          https://cirobuono1962-alt.github.io/meddispenser/
 * ============================================================
 *
 *  CONNESSIONI HARDWARE ESP32:
 *  STEP         GPIO18
 *  DIR          GPIO19
 *  EN           GPIO5
 *  HOME endstop GPIO23 + GND
 *  AIN1 TB6612  GPIO25
 *  AIN2 TB6612  GPIO26
 *  PWMA TB6612  GPIO27
 *  BUZZER       GPIO32
 *  LED          GPIO33
 *  PULSANTE     GPIO13 + GND
 *  SDA display  GPIO21
 *  SCL display  GPIO22
 * ============================================================
 *
 *  CHANGELOG v77:
 *  - FIX v68: display fisso durante navigazione, doppio click esce
 *  - FIX v69: buildManualSlotList legge /slots/ Firebase  S2-S5 inclusi
 *  - FIX v71: cache slot al boot, BTN2 istantaneo
 *  - FIX v72: log migliorato [EXT]/[FIS], slot mancanti/vuoti espliciti
 *  - FIX v72: lista slot con nomi nel log BTN2
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include <NTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define FW_VERSION "v214.0"

// ?? OTA Update ??
#define OTA_VERSION_URL "https://raw.githubusercontent.com/cirobuono1962-alt/meddispenser/main/version.json"
int otaHour = 3;
int otaMin  = 0;
bool otaCheckedToday = false;
long otaLastDay = -1;

//  DISPLAY 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool displayOk = false;

//  CREDENZIALI 
Preferences prefs;
DNSServer dnsServer;
char WIFI_SSID[64] = "";
char WIFI_PASS[64] = "";
const char* FIREBASE_HOST = "dispenser-famiglia-buono-4cd30-default-rtdb.europe-west1.firebasedatabase.app";

// Telegram
const char* TG_TOKEN    = "8730053593:AAEsyPQ5sdwPoKv03lybLtJleICV9dwCcyU";
char TG_CHAT_ID[24]  = "5097242921";
char TG_CHAT_ID2[24] = "8787926485";
int  TG_REMIND_MIN   = 5;
int  TG_ALERT_MIN    = 10;

// CallMeBot
char WA_PHONE[32]  = "%2B393925249858";
char WA_APIKEY[16] = "7046104";

//  NTP 
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

WebServer webServer(80);

//  LOG 
String logBuf = "";
void addLog(String msg) {
  Serial.println(msg);
  logBuf += msg + "\n";
  if (logBuf.length() > 2000) logBuf = logBuf.substring(logBuf.length() - 2000);
}

//  PIN 
#define STEP_PIN    18
#define DIR_PIN     19
#define EN_PIN       5
#define HOME_PIN    23
#define MOTOR_IN1   25
#define MOTOR_IN2   26
#define MOTOR_ENA   27
#define BUZZER_PIN  32
#define LED_R_PIN   4
#define LED_G_PIN   12
#define LED_B_PIN   2
#define BTN_PIN     13
#define COVER_PIN   14
#define LED_DROP    16  // LED blu punto caduta medicina (100 ohm a GND)
#define BTN2_PIN    33  // naviga medicine
#define FINECORSA_PIN 15 // fine corsa rientro cremagliera (NO, INPUT_PULLUP)

// ?? Imposta true se motore/sensori sono fisicamente collegati ??
// false = solo display collegato, salta home/motore al boot
#define HARDWARE_PRESENT true
                         // HIGH=cremagliera fuori, LOW=cremagliera rientrata

//  PARAMETRI MOTORE 
#define STEPS_PER_SLOT_DEFAULT  266
int STEPS_PER_SLOT = STEPS_PER_SLOT_DEFAULT;
#define NUM_SLOTS       10
#define STEP_DELAY_US_DEFAULT  15000
int STEP_DELAY_US = STEP_DELAY_US_DEFAULT;
#define DISPENSE_SPEED_DEFAULT  200
int DISPENSE_SPEED = DISPENSE_SPEED_DEFAULT;
#define DISPENSE_MS_DEFAULT    2000
int DISPENSE_MS = DISPENSE_MS_DEFAULT;
#define SETTLE_MS_DEFAULT      500
int SETTLE_MS = SETTLE_MS_DEFAULT;
#define RETRACT_MS_DEFAULT     500
int RETRACT_MS = RETRACT_MS_DEFAULT;
#define DISPENSE_PAUSE  300
#define MAX_SCHEDULES   30  // aumentato da 15  supporta pi?? pazienti con pi?? orari
int SLOT_MAX = 12;  // capacit?? massima per scomparto (configurabile da Firebase /config/slot_max)

//  TIMING 
#define POLL_MS     5000
#define SCHED_MS   10000
#define RELOAD_MS  60000
#define HB_MS      30000
#define TG_CHECK_MS 30000

//  SCHEDULE 
struct Schedule {
  char    medName[32];
  char    id[20];
  char    paziente[24];  // nome paziente associato all'orario
  char    startDate[12]; // data inizio terapia "YYYY-MM-DD"
  char    endDate[12];   // data fine terapia "YYYY-MM-DD" (vuoto = infinito)
  uint8_t hour, minute, slot, qty, freq, days;
  bool    active, valid;
  bool    external;
};
Schedule schedules[MAX_SCHEDULES];
int  scheduleCount = 0;
int  currentSlot   = 1;
// DEVICE ID
char   deviceId[12] = "";
String devPath      = "";

bool busy          = false;
char currentDispMed[32]     = "";  // medicina in erogazione (per display)
char currentDispPaziente[24]= "";  // paziente in erogazione (per display)
bool wifiWasDown   = false;
bool systemPaused  = false;
int  lastSummaryDay = -1;
bool coverOpen = false;
bool coverWasOpen = false;
bool loadingMode = false;
int  loadingSlot = 0;
int  loadingQty  = 0;
int  nightHomeHour = 3;
int  nightHomeMin  = 0;
int  lastNightHomeDay = -1;
unsigned long totalDispenses = 0;
unsigned long partialDispenses = 0;
bool ledBlinking = false;
int  ledMode=1;
int  ledModePrev=0;
int  ledFlashCnt=0;
unsigned long standbyStart=0;
bool ledState=false;
unsigned long lastLedMs=0;

//  NAVIGAZIONE MANUALE 
int  manualSelSlot = 0;
unsigned long lastBtn2Ms = 0;
unsigned long manualSelTime = 0;
int  manualSlotList[10];
int  manualSlotCount = 0;
int  manualSlotIdx = -1;   // -1 = non in navigazione
// Cache nomi slot  caricata una volta in background, usata da displayManualSelect
char slotNames[11][32];    // slotNames[1..10] = nome medicina nello slot
int  slotQtys[11];         // slotQtys[1..10]  = scorte correnti nello slot
bool slotCacheReady = false;
unsigned long lastSlotCacheMs = 0;
#define SLOT_CACHE_MS  600000  // ricarica cache ogni 10 minuti
// Variabili stato BTN2  globali per poter essere resettate da resetBtn2State()
bool btn2WasHigh    = true;
unsigned long btn2PressStart = 0;
bool btn2LongFired  = false;
bool btn2ResetNeeded = false;
bool finecorsaError = false;   // true = errore rientro cremagliera

bool externalPending = false;
unsigned long externalDisplayUntil=0;
#define LED_BLINK_ON_MS  4000
#define LED_BLINK_OFF_MS 500
int  summaryHour = 7;
int  summaryMin  = 30;
char summaryDest[10] = "both";
unsigned long lastAutoDispense = 0;
char lastFiredId[20] = "";
int  lastFiredHour = -1, lastFiredMin = -1;

//  BUZZER NON BLOCCANTE 
bool isBuzzing = false;
int bz_total=0, bz_done=0, bz_onMs=0, bz_offMs=0;
bool bz_isOn=false, bz_active=false;
unsigned long bz_last=0;

int getHour();
int getMin();

void setRGB(bool r,bool g,bool b){
  ledcWrite(LED_R_PIN, r?255:0);
  ledcWrite(LED_G_PIN, g ? 255 : 0);
  ledcWrite(LED_B_PIN, b?255:0);
}

void ledRGBUpdate(){
  unsigned long t=millis();
  if(ledMode!=ledModePrev){
    ledcWrite(LED_R_PIN, 0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
    digitalWrite(LED_DROP, LOW);
    lastLedMs=t; ledState=false; ledFlashCnt=0; ledModePrev=ledMode;
    if(ledMode==1) standbyStart=t;
  }
  switch(ledMode){
    case 1:
      { int hh = getHour();
        bool notturno = (hh >= 0 && hh < 6);
        if(notturno){
          ledcWrite(LED_R_PIN,0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN,0);
        } else if(t - standbyStart > 60000UL){
          // Arcobaleno fluido  hue scorre continuamente, luminosit?? costante
          static unsigned long lastRainbow = 0;
          static float hue = 0.0f;
          if (t - lastRainbow >= 20) {
            lastRainbow = t;
            hue += 0.12f; if (hue >= 360.0f) hue -= 360.0f;
          }
          float brightness = 0.55f; // luminosit?? costante  niente respiro
          float h6 = hue / 60.0f; int hi = (int)h6 % 6; float f = h6 - (int)h6;
          float q=brightness*(1.0f-f), tv=brightness*(1.0f-(1.0f-f));
          float r=0,g=0,b=0;
          switch(hi){
            case 0:r=brightness;g=tv;b=0;break;
            case 1:r=q;g=brightness;b=0;break;
            case 2:r=0;g=brightness;b=tv;break;
            case 3:r=0;g=q;b=brightness;break;
            case 4:r=tv;g=0;b=brightness;break;
            case 5:r=brightness;g=0;b=q;break;
          }
          ledcWrite(LED_R_PIN,(int)(r*255));
          ledcWrite(LED_G_PIN,(int)(g*255));
          ledcWrite(LED_B_PIN,(int)(b*255));
        } else {
          ledcWrite(LED_R_PIN,255); ledcWrite(LED_G_PIN,255); ledcWrite(LED_B_PIN,255);
        }
      }
      break;
    case 2:
      if(t-lastLedMs>=200){lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_R_PIN, 0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, ledState?255:0);}
      break;
    case 3:
      if(t-lastLedMs>=(ledState?300:150)){lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_R_PIN, 0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, ledState?255:0);
        digitalWrite(LED_DROP, ledState?HIGH:LOW);
      }
      break;
    case 4:
      { unsigned long el = t - lastLedMs;
        if(el < 333){
          ledcWrite(LED_R_PIN, 255); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        } else if(el < 666){
          ledcWrite(LED_R_PIN, 255); ledcWrite(LED_G_PIN,255); ledcWrite(LED_B_PIN, 255); // bianco fisso
        } else if(el < 999){
          ledcWrite(LED_R_PIN, 0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 255);
        } else {
          ledcWrite(LED_R_PIN, 0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
          ledMode=1; standbyStart=t;
        }
      }
      break;
    case 5:
      if(t-lastLedMs>=1000){lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        ledcWrite(LED_R_PIN, ledState?255:0);}
      break;
    case 6:
      if(t-lastLedMs>=500){
        lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        ledcWrite(LED_R_PIN, ledState?255:0);
      }
      break;
    case 7:
      if(t-lastLedMs>=200){
        lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        ledcWrite(LED_R_PIN, ledState?255:0);
      }
      break;
    case 9: // ALLARME fine corsa cremagliera: rosso 100ms
      if(t-lastLedMs>=100){
        lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        ledcWrite(LED_R_PIN, ledState?255:0);
      }
      break;
    case 10: // Rotazione carosello: rosso lampeggio veloce 150ms
      if(t-lastLedMs>=150){
        lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        ledcWrite(LED_R_PIN, ledState?255:0);
      }
      break;
    case 14: // Caricamento in attesa: bianco pulsante lento
      if(t-lastLedMs>=(unsigned long)(ledState?600:400)){
        lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_R_PIN,ledState?160:0);
        ledcWrite(LED_G_PIN,ledState?160:0);
        ledcWrite(LED_B_PIN,ledState?160:0);}
      break;
    case 8:
      if(t-lastLedMs>=(ledState?2000:1000)){
        lastLedMs=t; ledState=!ledState;
        ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
        ledcWrite(LED_R_PIN, ledState?255:0);
      }
      break;
    default:
      ledcWrite(LED_R_PIN, 0); ledcWrite(LED_G_PIN,0); ledcWrite(LED_B_PIN, 0);
      break;
  }
}

void buzzerStart(int times, int onMs, int offMs) {
  bz_total=times; bz_done=0;
  bz_onMs=onMs; bz_offMs=offMs;
  bz_isOn=true; bz_active=true;
  bz_last=millis(); isBuzzing=true;
  digitalWrite(BUZZER_PIN, HIGH);
}

void buzzerUpdate() {
  if (!bz_active) return;
  unsigned long now = millis();
  if (bz_isOn && now - bz_last >= (unsigned long)bz_onMs) {
    digitalWrite(BUZZER_PIN, LOW);
    bz_isOn=false; bz_last=now; bz_done++;
    if (bz_done >= bz_total) { bz_active=false; isBuzzing=false; }
  } else if (!bz_isOn && bz_active && now - bz_last >= (unsigned long)bz_offMs) {
    digitalWrite(BUZZER_PIN, HIGH);
    bz_isOn=true; bz_last=now;
  }
}

void buzzerAlert()    { buzzerStart(3, 200, 200); }
void buzzerReminder() { buzzerStart(6, 100, 100); }
void buzzerOk()       { buzzerStart(2, 80, 80);   }
void buzzerSlot()     {
  // Bip singolo pulito per avanzamento slot  reset forzato prima
  bz_active = false; isBuzzing = false; bz_done = 0;
  digitalWrite(BUZZER_PIN, LOW);
  delay(20); // pausa minima per reset fisico
  buzzerStart(1, 120, 0); // un solo bip da 120ms
}

//  PENDING TELEGRAM 
struct TgPending {
  char medName[32];
  char chatId[20];
  unsigned long dispenseTime;
  unsigned long reminderSent;
  bool confirmed;
  bool valid;
  char logKey[24];
};
#define MAX_PENDING 5
TgPending tgPending[MAX_PENDING];
unsigned long lastPoll=0, lastSched=0, lastReload=0, lastHB=0, lastTgCheck=0;

//  FORWARD DECLARATIONS 
void initMotors();
void homeCarousel();
void gotoSlot(int t);
void stepNEMA(int s, bool d);
void enableNEMA(bool e);
void dispense(int qty);
void motorCC(bool fwd, int spd);
void stopMotorCC();
bool wifiConnect();
String fbGet(String path);
bool   fbPut(String path, String json);
String parseField(String& json, const char* field);
void   pushHB();
void   loadSchedules();
void   loadMotorConfig();
void   runScheduler();
void   processCommand(String& json);
String logDispense(int slot, int qty, bool ok, const char* name, bool manual=false, const char* paziente="");
int    getHour();
int    getMin();
int    getDow();
int    getDay();
int    getMon();
bool   tgSendMsg(const char* chatId, String msg, bool withBtn);
bool   waCall(String msg);
void   tgCheckConfirm();
bool   isDST(int day, int month, int dow);
void   updateNTPOffset();
void   updateDisplay();
void   buildManualSlotList();
void   loadSlotCache();
void   resetBtn2State();  // resetta stato BTN2 dopo operazioni bloccanti
void   checkCremaglieraAtBoot();
void   displayVuotoAlarm(int slot, const char* medName, int durationMs);
bool   isSchedActiveToday(Schedule& s);
void   checkAgendaNotify();
String getTodayString();
void   markManualTaken(int slot);
bool   isManualTaken(int slot);
long   daysFromStart(const char* dateStr);
void   displayManualSelect(int slot);
void   displayLoadingSlot(int slot);
void   displayLoadingReady();
void   displayExternalMed(const char* medName, const char* paziente, int h, int m);
void   checkCover();
void   saveMotorToFlash();
void   loadMotorFromFlash();
void   loadSettings();
void   updateSlotQty(int slot, const char* name, int dispensed);
void   sendMorningSummary();
void   checkOTA();
void   initLoadingQty();
void   nightHomeCheck();
int    findAlternativeSlot(const char* medName, int excludeSlot);
void   wifiPortal();

//  ORA LEGALE 
bool isDST(int day, int month, int dow) {
  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;
  int lastSun = day - dow;
  if (month == 3) return day >= lastSun && lastSun >= 25;
  if (month == 10) return !(day >= lastSun && lastSun >= 25);
  return false;
}

void updateNTPOffset() {
  unsigned long epoch = timeClient.getEpochTime();
  if (epoch < 1000000000UL) return;
  unsigned long d = epoch / 86400L;
  int dow = (d + 4) % 7;
  int y = 1970;
  while(true){int diy=(y%4==0&&(y%100!=0||y%400==0))?366:365;if(d<(unsigned long)diy)break;d-=diy;y++;}
  int dm[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(y%4==0&&(y%100!=0||y%400==0))dm[1]=29;
  int m=1; while(d>=(unsigned long)dm[m-1]){d-=dm[m-1];m++;}
  int day = d + 1;
  long offset = isDST(day, m, dow) ? 7200 : 3600;
  timeClient.setTimeOffset(offset);
}

//  DISPLAY 
void updateDisplay() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 4);
  display.print("MedDispenser " FW_VERSION);

  display.setTextSize(2);
  display.setCursor(0, 16);
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", getHour(), getMin());
  display.print(timeStr);

  display.setTextSize(1);
  if (WiFi.isConnected()) {
    int rssi = WiFi.RSSI();
    int bars = 0;
    if      (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else                 bars = 1;
    for (int b=0; b<4; b++) {
      int barH = 3 + b*3;
      int barX = 102 + b*6;
      int barY = 28 - barH;
      if (b < bars) display.fillRect(barX, barY, 4, barH, SSD1306_WHITE);
      else          display.drawRect(barX, barY, 4, barH, SSD1306_WHITE);
    }
  } else {
    display.setCursor(108, 16);
    display.print("X");
  }

  display.setCursor(68, 16);
  if (busy) {
    display.print("EROGANDO");
    // Mostra paziente e medicina corrente (riga 32)
    display.setCursor(0, 32);
    display.setTextSize(1);
    if (strlen(currentDispPaziente) > 0) {
      char pazShort[12]; strncpy(pazShort, currentDispPaziente, 11); pazShort[11]=0;
      display.print(pazShort);
      display.print(":");
    }
    char medShortBusy[14]; strncpy(medShortBusy, currentDispMed, 13); medShortBusy[13]=0;
    display.print(medShortBusy);
  } else {
    display.print("S:");
    display.print(currentSlot);
    display.print("/10");
  }

  display.setCursor(0, 40);
  // Se c'?? un pending non confermato mostra paziente + medicina
  bool showPending = false;
  for (int pi=0; pi<MAX_PENDING; pi++) {
    if (tgPending[pi].valid && !tgPending[pi].confirmed) {
      // Mostra nome paziente grande + medicina
      if (strlen(currentDispPaziente) > 0) {
        display.setTextSize(1);
        display.print(">> ");
        char pazDisp[18]; strncpy(pazDisp, currentDispPaziente, 17); pazDisp[17]=0;
        display.print(pazDisp);
        display.setCursor(0, 52);
        char medDisp[18]; strncpy(medDisp, currentDispMed, 17); medDisp[17]=0;
        display.print(medDisp);
      } else {
        display.print("!! CONFERMA !!     ");
      }
      showPending = true;
      break;
    }
  }
  if (!showPending) {
    // Nessun pending  mostra prossimo orario
    int nowMin2 = getHour()*60 + getMin();
    int bestDiff = 9999;
    int bestIdx = -1;
    for (int i=0; i<scheduleCount; i++) {
      if (!schedules[i].valid || !schedules[i].active) continue;
      int sMin = schedules[i].hour*60 + schedules[i].minute;
      int diff = sMin - nowMin2;
      if (diff > 0 && diff < bestDiff) { bestDiff=diff; bestIdx=i; }
    }
    if (bestIdx == -1) {
      for (int i=0; i<scheduleCount; i++) {
        if (!schedules[i].valid || !schedules[i].active) continue;
        int sMin = schedules[i].hour*60 + schedules[i].minute;
        if (sMin < bestDiff) { bestDiff=sMin; bestIdx=i; }
      }
    }
    if (bestIdx >= 0) {
      char medShort[10]; strncpy(medShort, schedules[bestIdx].medName, 9); medShort[9]=0;
      char t[6]; sprintf(t, "%02d:%02d", schedules[bestIdx].hour, schedules[bestIdx].minute);
      display.print(">");
      display.print(medShort);
      display.print(" ");
      display.print(t);
    } else {
      display.print("> Nessun orario");
    }
  }

  display.setCursor(0, 52);
  bool hasPending = false;
  for (int i=0; i<MAX_PENDING; i++) {
    if (tgPending[i].valid && !tgPending[i].confirmed) { hasPending=true; break; }
  }
  if (coverOpen && loadingSlot == 0) {
    display.print("!! APR.COPERT.->BTN");
  } else if (coverOpen && loadingSlot > 0) {
    return;
  } else if (hasPending) {
    display.print("!! CONFERMA !!     ");
  } else {
    display.print(WiFi.localIP().toString());
  }

  display.display();
}

//  SETUP 
// Genera un device_id casuale tipo "MD-A3F2"
String generateDeviceId() {
  const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String id = "MD-";
  randomSeed(ESP.getEfuseMac());
  for (int i = 0; i < 4; i++) id += chars[random(0, 32)];
  return id;
}

void loadDeviceId() {
  Preferences prefs;
  prefs.begin("meddispenser", false);
  String saved = prefs.getString("device_id", "");
  if (saved.length() == 0) {
    // Prima accensione  genera e salva
    saved = generateDeviceId();
    prefs.putString("device_id", saved);
    Serial.println("[DEVICE] Nuovo device_id generato: " + saved);
  }
  saved.toCharArray(deviceId, 12);
  devPath = "/devices/" + saved;
  prefs.end();
  Serial.println("[DEVICE] device_id: " + String(deviceId));
  Serial.println("[DEVICE] Firebase path: " + devPath);
}

void setup() {
  Serial.begin(115200);
  loadDeviceId();
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  ledcAttach(LED_R_PIN, 1000, 8); ledcWrite(LED_R_PIN, 0);
  ledcAttach(LED_G_PIN, 1000, 8); ledcWrite(LED_G_PIN, 0);
  ledcAttach(LED_B_PIN, 1000, 8); ledcWrite(LED_B_PIN, 0);
  pinMode(EN_PIN, OUTPUT);     digitalWrite(EN_PIN, HIGH);

  Serial.begin(115200);

  Wire.begin(21, 22);
  delay(100);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    displayOk = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("MedDispenser v214.0");
    display.println("Avvio in corso...");
    display.display();
  }

  initMotors();
  memset(tgPending, 0, sizeof(tgPending));
  addLog("[BOOT] MedDispenser ESP32 " FW_VERSION);

  if (wifiConnect()) {
    webServer.on("/", [](){
      String colored = "";
      String lb = logBuf;
      int start = 0;
      while (start < (int)lb.length()) {
        int nl = lb.indexOf('\n', start);
        if (nl == -1) nl = lb.length();
        String line = lb.substring(start, nl);
        String color = "#cdd6f4";
        if (line.indexOf("[BOOT]") != -1)              color = "#89b4fa";
        else if (line.indexOf("[WiFi]") != -1)         color = "#89dceb";
        else if (line.indexOf("[NTP]") != -1)          color = "#89dceb";
        else if (line.indexOf("[WEB]") != -1)          color = "#89dceb";
        else if (line.indexOf("[SCHED] Erogazione") != -1) color = "#a6e3a1";
        else if (line.indexOf("[SCHED]") != -1)        color = "#94e2d5";
        else if (line.indexOf("[DISP]") != -1)         color = "#a6e3a1";
        else if (line.indexOf("[TG]") != -1)           color = "#89b4fa";
        else if (line.indexOf("[CALL]") != -1)         color = "#cba6f7";
        else if (line.indexOf("[BTN]") != -1)          color = "#f9e2af";
        else if (line.indexOf("[CMD]") != -1)          color = "#fab387";
        else if (line.indexOf("[CFG]") != -1)          color = "#f38ba8";
        else if (line.indexOf("[HOME]") != -1)         color = "#f9e2af";
        else if (line.indexOf("[STOP]") != -1)         color = "#f38ba8";
        else if (line.indexOf("[READY]") != -1)        color = "#a6e3a1";
        else if (line.indexOf("ERRORE") != -1)         color = "#f38ba8";
        colored += "<span style='color:" + color + "'>" + line + "</span>\n";
        start = nl + 1;
      }
      String h = "<html><head><meta charset='utf-8'>"
                 "<meta http-equiv='refresh' content='5'>"
                 "<style>"
                 "body{font-family:'Courier New',monospace;background:#1e1e2e;color:#cdd6f4;margin:0;padding:0;}"
                 ".header{background:#181825;padding:12px 16px;border-bottom:1px solid #313244;display:flex;align-items:center;justify-content:space-between;position:sticky;top:0;}"
                 ".title{color:#89b4fa;font-size:16px;font-weight:bold;}"
                 ".status{font-size:11px;color:#6c7086;}"
                 ".log{padding:12px 16px;font-size:12px;line-height:1.6;white-space:pre-wrap;word-break:break-all;}"
                 ".footer{background:#181825;padding:8px 16px;border-top:1px solid #313244;font-size:10px;color:#6c7086;text-align:center;}"
                 "</style>"
                 "<script>window.onload=()=>window.scrollTo(0,document.body.scrollHeight);</script>"
                 "</head><body>"
                 "<div class='header'>"
                 "<div class='title'>? MedDispenser ESP32 " + String(FW_VERSION) + "</div>"
                 "<div class='status'>IP: " + WiFi.localIP().toString() + " | RSSI: " + String(WiFi.RSSI()) + " dBm | Aggiornamento: 5s</div>"
                 "</div>"
                 "<div class='log'>" + colored + "</div>"
                 "<div class='footer'>ESP32 | Firebase: dispenser-famiglia-buono-4cd30 | Scomparto: " + String(currentSlot) + "/10</div>"
                 "</body></html>";
      webServer.send(200, "text/html", h);
    });
    webServer.begin();
    addLog("[WEB] http://" + WiFi.localIP().toString());
  }

  loadMotorFromFlash();
  checkCremaglieraAtBoot(); // verifica cremagliera rientrata prima di girare il carosello
  homeCarousel();
  delay(200);
  Wire.begin(21, 22);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    displayOk = true;
    addLog("[DISP] Display reinizializzato dopo home");
  }

  if (WiFi.isConnected()) {
    loadSettings();
    // Notifica OTA completato se la versione e' quella attesa
    String lastOtaVer = fbGet("/config/settings/otaLastVer.json");
    lastOtaVer.replace("\"", "");
    if (lastOtaVer.length() > 0 && lastOtaVer != FW_VERSION) {
      // versione diversa - notifica successo
      tgSendMsg(TG_CHAT_ID, "MedDispenser " + String(deviceId) + ": aggiornato a " FW_VERSION " con successo!", false);
      fbPut("/config/settings/otaLastVer", """ FW_VERSION """);
    } else if (lastOtaVer.length() == 0) {
      fbPut("/config/settings/otaLastVer", """ FW_VERSION """);
    }
    loadMotorConfig();
    loadSchedules();
    loadSlotCache();   // carica nomi slot in cache - BTN2 sara' istantaneo
    // Invia ID dispositivo via Telegram al primo avvio
    {
      Preferences prefsId;
      prefsId.begin("meddispenser", false);
      bool announced = prefsId.getBool("announced", false);
      prefsId.end();
      if (!announced && strlen(deviceId) > 0) {
        String appUrl = "https://cirobuono1962-alt.github.io/meddispenser/?device=" + String(deviceId);
        String msg = "MedDispenser " + String(deviceId) + " online!\n";
        msg += "URL app:\n" + appUrl + "\n\n";
        msg += "Firmware: " FW_VERSION "\n";
        msg += "IP: " + WiFi.localIP().toString();
        tgSendMsg(TG_CHAT_ID, msg, false);
        addLog("[BOOT] ID inviato via Telegram: " + String(deviceId));
        Preferences prefsId2;
        prefsId2.begin("meddispenser", false);
        prefsId2.putBool("announced", true);
        prefsId2.end();
      }
    }
    // Carica stato pausa da Firebase
    String pausedVal = fbGet(devPath + "/config/paused.json");
    if (pausedVal == "true") {
      systemPaused = true;
      ledMode = 3;
      addLog("[BOOT] Sistema in PAUSA (da Firebase)");
    }
    pushHB();
  }

  addLog("[BOOT] Test buzzer e LED...");
  digitalWrite(BUZZER_PIN,HIGH);
  ledcWrite(LED_R_PIN, 255); delay(300); ledcWrite(LED_R_PIN, 0);
  ledcWrite(LED_G_PIN,255); delay(300); ledcWrite(LED_G_PIN,0);
  ledcWrite(LED_B_PIN, 255); delay(300); ledcWrite(LED_B_PIN, 0);
  digitalWrite(BUZZER_PIN, LOW); setRGB(0,0,0); delay(100);
  digitalWrite(BUZZER_PIN,HIGH);
  ledcWrite(LED_R_PIN, 255); delay(300); ledcWrite(LED_R_PIN, 0);
  ledcWrite(LED_G_PIN,255); delay(300); ledcWrite(LED_G_PIN,0);
  ledcWrite(LED_B_PIN, 255); delay(300); ledcWrite(LED_B_PIN, 0);
  digitalWrite(BUZZER_PIN, LOW); addLog("[BOOT] Test completato");

  if (displayOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("MedDispenser v214.0");
    display.println("Sistema pronto!");
    display.print("IP: ");
    display.println(WiFi.localIP().toString());
    display.display();
  }

  addLog("[READY] Sistema pronto");
  ledMode=WiFi.isConnected()?1:8;
}

//  LOOP 
void loop() {
  ledRGBUpdate();
  webServer.handleClient();
  buzzerUpdate();
  timeClient.update();

  unsigned long now = millis();

  // Polling comandi Firebase
  if (now - lastPoll >= POLL_MS && !busy) {
    lastPoll = now;
    String json = fbGet(devPath + "/command.json");
    if (json.length() > 2 && json != "null") {
      String done = parseField(json, "done");
      if (done == "false") {
        processCommand(json);
        fbPut(devPath + "/command/done", "true");
      }
    }
  }

  // Controlla coperchio
  static unsigned long lastCoverCheck = 0;
  if (millis() - lastCoverCheck >= 100) {
    lastCoverCheck = millis();
    checkCover();
  }

  // Scheduler
  if (now - lastSched >= SCHED_MS && !busy) {
    lastSched = now;
    sendMorningSummary();
    nightHomeCheck();
    runScheduler();
  }

  // OTA e settings reload: sempre, anche se busy
  static unsigned long lastSettingsReload = 0;
  if (millis() - lastSettingsReload > 600000UL) {
    lastSettingsReload = millis();
    loadSettings();
  }
  checkOTA();

  // Ricarica schedules ogni 60s
  if (now - lastReload >= RELOAD_MS) {
    lastReload = now;
    loadSchedules();
    // Ricostruisce lista slot dopo reload schedules (se cache gi?? pronta)
    if (slotCacheReady) buildManualSlotList();
  }
  // Ricarica cache slot ogni 10 minuti
  // NON ricaricare se: navigazione BTN2 attiva, busy, coperchio aperto
  if (now - lastSlotCacheMs >= SLOT_CACHE_MS && WiFi.isConnected() && !busy && !coverOpen && manualSlotIdx == -1) {
    loadSlotCache();
    resetBtn2State();
  }

  // Heartbeat
  if (now - lastHB >= HB_MS) {
    lastHB = now;
    updateNTPOffset();
    pushHB();
  }

  // Polling Telegram
  if (now - lastTgCheck >= TG_CHECK_MS) {
    lastTgCheck = now;
    tgCheckConfirm();
  }

  // Buzzer spento a riposo
  if (!isBuzzing) {
    digitalWrite(BUZZER_PIN, LOW);
  }

  // PULSANTE 1 (GPIO13): conferma / eroga
  static unsigned long lastBtnMs = 0;
  static unsigned long btn1PressStart = 0;
  static bool btn1WasLow = false;
  bool btn1Now = (digitalRead(BTN_PIN) == LOW);

  // MODALITA CARICAMENTO: click breve=+1, hold=conferma
  if (loadingMode && coverOpen) {
    if (btn1Now && !btn1WasLow) {
      btn1PressStart = millis(); // inizio pressione
    }
    // Primo slot: click breve avvia home
    if (loadingSlot == 0 && !btn1Now && btn1WasLow && (millis()-btn1PressStart) < 1800) {
      if (millis()-lastBtnMs > 300) {
        lastBtnMs = millis();
        digitalWrite(BUZZER_PIN, HIGH); delay(25); digitalWrite(BUZZER_PIN, LOW);
        addLog("[LOAD] Home - scomparto 1");
        homeCarousel();
        delay(200);
        Wire.begin(21, 22);
        display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
        resetBtn2State();
        loadingSlot = 1;
        initLoadingQty();
        ledMode = 14;
        displayLoadingSlot(1);
        buzzerSlot();
      }
    }
    // Slot > 0: rilascio breve = +1
    if (loadingSlot > 0 && !btn1Now && btn1WasLow && (millis()-btn1PressStart) < 1800 && millis()-lastBtnMs > 150) {
      lastBtnMs = millis();
      if (loadingQty < SLOT_MAX) loadingQty++;
      displayLoadingSlot(loadingSlot);
      digitalWrite(BUZZER_PIN, HIGH); delay(15); digitalWrite(BUZZER_PIN, LOW);
    }
    // Slot > 0: hold >= 1800ms = conferma e avanza
    if (loadingSlot > 0 && btn1Now && btn1WasLow && (millis()-btn1PressStart) >= 1800) {
      btn1PressStart = 9999999UL; // evita doppio trigger
      lastBtnMs = millis();
      digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW);
      addLog("[LOAD] S" + String(loadingSlot) + " qt=" + String(loadingQty));
      String fpath = "/slots/slot_" + String(loadingSlot) + "/qty";
      fbPut(fpath, String(loadingQty));
      if (loadingSlot >= 1 && loadingSlot <= NUM_SLOTS) slotQtys[loadingSlot] = loadingQty;
      buzzerStart(2, 100, 80);
      if (loadingSlot < NUM_SLOTS) {
        loadingSlot++;
        initLoadingQty();
        gotoSlot(loadingSlot);
        ledMode = 14;
        displayLoadingSlot(loadingSlot);
        buzzerSlot();
      } else {
        addLog("[LOAD] Caricamento completato!");
        buzzerStart(3, 100, 100);
        ledMode = 1;
      }
    }
    btn1WasLow = btn1Now;
    goto skipBtn1; // salta gestione normale BTN1 ma continua con BTN2
  }

  if (digitalRead(BTN_PIN) == LOW && millis()-lastBtnMs > 300) {
    delay(30);
    if (digitalRead(BTN_PIN) == LOW) {
      lastBtnMs = millis();
      // Bip immediato di conferma contatto
      digitalWrite(BUZZER_PIN, HIGH); delay(25); digitalWrite(BUZZER_PIN, LOW);

            // Doppio click BTN1 entro 800ms = annulla selezione
      static unsigned long lastBtn1Click = 0;
      unsigned long nowBtn = millis();
      if (manualSelSlot > 0 && (nowBtn - lastBtn1Click) < 800) {
        manualSelSlot = 0;
        manualSlotIdx = -1;
        ledMode = 1;
        updateDisplay();
        addLog("[BTN] Selezione annullata (doppio click)");
        lastBtn1Click = 0;
        delay(300);
        return;
      }
      lastBtn1Click = nowBtn;

      // Se c'e' uno scomparto selezionato da BTN2, eroga quello
      if (manualSelSlot > 0) {
        int sl = manualSelSlot;
        manualSelSlot = 0;
        manualSlotIdx = -1;  // resetta navigazione dopo erogazione
        if (finecorsaError) {
          addLog("[BTN] Prelievo bloccato: errore fine corsa attivo");
          tgSendMsg(TG_CHAT_ID, "?? Prelievo manuale bloccato: errore fine corsa. Riavviare dopo verifica.", false);
          return;
        }
        // Leggi nome da cache locale
        String mn = "";
        if (sl >= 1 && sl <= NUM_SLOTS && strlen(slotNames[sl]) > 0) {
          mn = String(slotNames[sl]);
        }
        if (mn.length() == 0) {
          for (int i=0; i<scheduleCount; i++) {
            if (schedules[i].valid && schedules[i].slot == sl) {
              mn = String(schedules[i].medName); break;
            }
          }
        }
        // Controlla scorte - failover se vuoto (stesso comportamento automatico)
        int useSlot = sl;
        String slotJson = fbGet("/slots/slot_" + String(sl) + ".json");
        int currentQty = 12;
        if (slotJson != "null" && slotJson.length() > 2) {
          String qtyStr = parseField(slotJson, "qty");
          if (qtyStr.length() > 0) currentQty = qtyStr.toInt();
        }
        if (currentQty <= 0 && mn.length() > 0) {
          int altSlot = findAlternativeSlot(mn.c_str(), sl);
          if (altSlot > 0) {
            addLog("[BTN] Scomparto " + String(sl) + " vuoto - failover su S" + String(altSlot));
            String altMsg = "Scomparto " + String(sl) + " vuoto! Prelievo da scomparto " + String(altSlot) + " (" + mn + ")";
            tgSendMsg(TG_CHAT_ID, altMsg, false);
            useSlot = altSlot;
          } else {
            addLog("[BTN] Scomparto " + String(sl) + " vuoto - nessun alternativo");
            String blkMsg = "?? Scomparto " + String(sl) + " (" + mn + ") VUOTO! Nessun alternativo disponibile.";
            tgSendMsg(TG_CHAT_ID, blkMsg, false);
            ledMode = 6;
            buzzerStart(5, 200, 200);
            displayVuotoAlarm(sl, mn.c_str(), 5000);
            ledMode = 1;
            busy = false;
            return;
          }
        }
        addLog("[BTN] Prelievo manuale scomparto " + String(useSlot) + " (richiesto S" + String(sl) + ")");
        busy=true; ledMode=2;
        gotoSlot(useSlot); // gotoSlot imposta ledMode=3 automaticamente
        dispense(1);
        strncpy(currentDispMed, mn.c_str(), 31);
        strncpy(currentDispPaziente, "", 1);
        logDispense(useSlot, 1, true, mn.c_str(), true);
        markManualTaken(useSlot); // segna come prelevato manualmente oggi
        updateSlotQty(useSlot, mn.c_str(), 1);
        String tgMsg = "Prelievo manuale: " + mn + " da scomparto " + String(useSlot);
        if (useSlot != sl) tgMsg += " (failover da S" + String(sl) + ")";
        tgMsg += ". Rispondi OK per confermare.";
        tgSendMsg(TG_CHAT_ID, tgMsg, false);
        buzzerOk();
        for (int pi=0; pi<MAX_PENDING; pi++) {
          if (!tgPending[pi].valid) {
            mn.toCharArray(tgPending[pi].medName, 31);
            strncpy(tgPending[pi].chatId, TG_CHAT_ID, 19);
            tgPending[pi].dispenseTime = timeClient.getEpochTime();
            tgPending[pi].reminderSent = 0;
            tgPending[pi].confirmed = false;
            tgPending[pi].valid = true;
            break;
          }
        }
        ledMode=3;
        busy=false; pushHB();
        return;
      }

      bool found = false;
      int confirmedCount = 0;
      for (int i=0; i<MAX_PENDING; i++) {
        if (tgPending[i].valid && !tgPending[i].confirmed) {
          tgPending[i].confirmed = true;
          currentDispMed[0]=0; currentDispPaziente[0]=0;
          if (tgPending[i].logKey[0] != 0) {
            fbPut("/log/" + String(tgPending[i].logKey) + "/confirmed", "true");
            fbPut("/log/" + String(tgPending[i].logKey) + "/confirmedAt", String(timeClient.getEpochTime()));
            fbPut("/log/" + String(tgPending[i].logKey) + "/confirmedHour", String(getHour()));
            fbPut("/log/" + String(tgPending[i].logKey) + "/confirmedMin", String(getMin()));
          }
          tgSendMsg(tgPending[i].chatId, "Confermato con pulsante fisico! Buona salute", false);
          memset(&tgPending[i], 0, sizeof(TgPending));
          externalPending = false;
          lastTgCheck=millis();
          externalDisplayUntil=0;
          confirmedCount++;
          found = true;
        }
      }
      if (found) {
        ledMode = 4;
        addLog("[BTN] Confermate " + String(confirmedCount) + " medicine");
        bz_active=false; isBuzzing=false; digitalWrite(BUZZER_PIN,LOW);
        delay(20); buzzerOk();
        delay(400); updateDisplay();
      } else {
        addLog("[BTN] Premuto (nessuna conferma pendente)");
      }
      delay(300);
      unsigned long t = millis();
      while (digitalRead(BTN_PIN) == LOW && millis()-t < 3000) delay(10);
      // Hold lungo (>2s) in MODALITA CARICAMENTO = conferma qty e avanza
      unsigned long held = millis() - t;
      if (loadingMode && coverOpen && loadingSlot > 0 && held >= 1800) {
        addLog("[LOAD] S" + String(loadingSlot) + " confermato qt=" + String(loadingQty));
        {
          String path = "/slots/slot_" + String(loadingSlot) + "/qty";
          fbPut(path, String(loadingQty));
          if (loadingSlot >= 1 && loadingSlot <= NUM_SLOTS) slotQtys[loadingSlot] = loadingQty;
        }
        buzzerStart(2, 100, 80);
        if (loadingSlot < NUM_SLOTS) {
          loadingSlot++;
          initLoadingQty();
          gotoSlot(loadingSlot);
          ledMode = 14;
          displayLoadingSlot(loadingSlot);
          buzzerSlot();
        } else {
          addLog("[LOAD] Caricamento completato!");
          buzzerStart(3, 100, 100);
          ledMode = 1;
        }
      }
    }
  }

  skipBtn1: ;

  //  PULSANTE 2 (GPIO33): naviga medicine per prelievo manuale 
  // CLICK BREVE   avanza alla medicina successiva (display resta fisso)
  // TIENI PREMUTO 1s  esce dalla navigazione, display torna normale
  // Reset stato dopo operazioni bloccanti
  if (btn2ResetNeeded) {
    btn2ResetNeeded = false;
    btn2WasHigh = (digitalRead(BTN2_PIN) == HIGH);
    btn2PressStart = 0;
    btn2LongFired = false;
    addLog("[BTN2] Stato resettato");
  }

  bool btn2Now = (digitalRead(BTN2_PIN) == HIGH);

  if (btn2WasHigh && !btn2Now) {
    // Fronte discendente: inizio pressione
    btn2PressStart = millis();
    digitalWrite(BUZZER_PIN, HIGH); delay(25); digitalWrite(BUZZER_PIN, LOW);
    btn2LongFired = false;
    if (loadingMode && coverOpen && loadingSlot > 0) {
      if (loadingQty > 0) loadingQty--;
      displayLoadingSlot(loadingSlot);
    }
  }

  if (!btn2Now && !btn2LongFired) {
    // Pulsante tenuto: controlla se supera 1s
    if (millis() - btn2PressStart >= 1000 && !busy && !coverOpen) {
      btn2LongFired = true;
      if (manualSlotIdx != -1) {
        // LONG PRESS: esci dalla navigazione
        manualSelSlot = 0;
        manualSlotIdx = -1;
        ledMode = 1;
        updateDisplay();
        addLog("[BTN2] Uscita navigazione (pressione lunga)");
      }
    }
  }

  if (!btn2WasHigh && btn2Now) {
    // Fronte ascendente: rilascio pulsante
    unsigned long pressDur = millis() - btn2PressStart;
    if (!btn2LongFired && pressDur < 800 && pressDur > 30 && !busy && !coverOpen) {
      // CLICK BREVE: avanza slot
      lastBtn2Ms = millis();
      // lista gi?? pronta dalla cache caricata al boot
      if (!slotCacheReady) buildManualSlotList(); // sicurezza
      if (manualSlotCount > 0) {
        manualSlotIdx = (manualSlotIdx + 1) % manualSlotCount;
        manualSelSlot = manualSlotList[manualSlotIdx];
        manualSelTime = millis();
        displayManualSelect(manualSelSlot);
        ledMode = 3;
        addLog("[BTN2] Selezionato scomparto " + String(manualSelSlot));
      } else {
        addLog("[BTN2] Nessuno slot disponibile");
      }
    }
  }

  btn2WasHigh = btn2Now;

  // WiFi watchdog
  if (!WiFi.isConnected()) {
    if (!wifiWasDown) {
      wifiWasDown=true; ledMode=8;
      addLog("[WiFi] DISCONNESSO!");
      buzzerStart(5, 100, 100);
    }
    addLog("[WiFi] Tentativo riconnessione...");
    WiFi.reconnect();
    delay(3000);
  } else if (wifiWasDown) {
    wifiWasDown=false; ledMode=1;
    addLog("[WiFi] Riconnesso! IP: " + WiFi.localIP().toString());
    static bool firstConn = true;
    if (firstConn && strlen(deviceId) > 0) {
      firstConn = false;
      delay(500);
      String appUrl = "https://cirobuono1962-alt.github.io/meddispenser/?device=" + String(deviceId);
      tgSendMsg(TG_CHAT_ID, "MedDispenser " + String(deviceId) + " online!\nURL app:\n" + appUrl, false);
    }

    buzzerStart(2, 200, 100);
    tgSendMsg(TG_CHAT_ID, "MedDispenser riconnesso al WiFi!", false);
    pushHB();
  }

  // Aggiorna display ogni 5 secondi
  // NON sovrascrive se: promemoria esterno attivo O navigazione manuale attiva
  static unsigned long lastDisplay = 0;
  if (now - lastDisplay >= 5000) {
    lastDisplay = now;
    if (!externalPending && manualSlotIdx == -1) updateDisplay();
  }

  // Check notifiche agenda ogni 30 secondi
  static unsigned long lastAgendaCheck = 0;
  if (now - lastAgendaCheck > 30000UL) {
    lastAgendaCheck = now;
    checkAgendaNotify();
  }
}

//  MOTORI 
void initMotors() {
  pinMode(STEP_PIN, OUTPUT); pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  pinMode(HOME_PIN, INPUT_PULLUP);
  pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_ENA, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  ledcAttach(LED_R_PIN, 1000, 8); ledcWrite(LED_R_PIN, 0);
  ledcAttach(LED_G_PIN, 1000, 8); ledcWrite(LED_G_PIN, 0);
  ledcAttach(LED_B_PIN, 1000, 8); ledcWrite(LED_B_PIN, 0);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(COVER_PIN, INPUT_PULLUP);
  pinMode(FINECORSA_PIN, INPUT_PULLUP); // GPIO15  pullup interno, NO tra pin e GND
  delay(100);
  coverOpen = HARDWARE_PRESENT ? (digitalRead(COVER_PIN) == HIGH) : false;
  if (coverOpen) addLog("[COVER] Boot: coperchio APERTO");
  enableNEMA(false); stopMotorCC();
  bz_active=false; bz_isOn=false; isBuzzing=false;
}

void enableNEMA(bool en) { digitalWrite(EN_PIN, en ? LOW : HIGH); }

void stepNEMA(int steps, bool dir) {
  digitalWrite(DIR_PIN, dir ? HIGH : LOW);
  for (int i=0; i<steps; i++) {
    digitalWrite(STEP_PIN, HIGH); delayMicroseconds(STEP_DELAY_US);
    digitalWrite(STEP_PIN, LOW);  delayMicroseconds(STEP_DELAY_US);
    if (i%10==0) yield();
  }
}

// Verifica al boot che la cremagliera sia rientrata (GPIO15=LOW)
// Se ?? fuori (HIGH), aziona il motore indietro fino al rientro o timeout 5s
// DEVE essere chiamata PRIMA di homeCarousel() per non rischiare blocchi
void checkCremaglieraAtBoot() {
  if (!HARDWARE_PRESENT) { addLog("[BOOT] Skip cremagliera (no hardware)"); return; }
  addLog("[BOOT] Verifica posizione cremagliera...");

  // Mostra sul display
  if (displayOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Verifica cremagliera");
    display.println("in corso...");
    display.display();
  }

  if (digitalRead(FINECORSA_PIN) == LOW) {
    // Cremagliera gi?? rientrata  tutto ok
    addLog("[BOOT] Cremagliera OK (gia' rientrata)");
    if (displayOk) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Cremagliera OK");
      display.display();
    }
    return;
  }

  // Cremagliera fuori  rientrala prima di procedere
  addLog("[BOOT] Cremagliera fuori! Rientro in corso...");
  if (displayOk) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.println("!! Cremagliera fuori");
    display.println("Rientro in corso...");
    display.display();
  }

  ledMode = 10; // rosso durante rientro al boot
  motorCC(false, DISPENSE_SPEED); // motore indietro
  unsigned long tStart = millis();
  bool ok = false;
  while (millis() - tStart < 5000UL) {
    if (digitalRead(FINECORSA_PIN) == LOW) {
      ok = true;
      break;
    }
    ledRGBUpdate();
    buzzerUpdate();
    yield();
    delay(5);
  }
  stopMotorCC();

  if (ok) {
    addLog("[BOOT] Cremagliera rientrata OK (" + String(millis()-tStart) + "ms)");
    if (displayOk) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Cremagliera OK");
      display.display();
    }
    delay(300); // pausa assestamento
  } else {
    // Timeout - avvisa via log ma NON blocca il boot (potrebbe essere sensore non collegato)
    addLog("[BOOT] WARN: cremagliera non rientrata al boot (sensore scollegato?)");
    // finecorsaError rimane false per non bloccare le erogazioni
    if (displayOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("!! ALLARME !!");
      display.println("Cremagliera non");
      display.println("rientrata!");
      display.println("Verificare motore");
      display.display();
    }
    buzzerStart(10, 300, 200);
    // Attendi 5s con LED e buzzer prima di procedere (non bloccare il boot)
    unsigned long alertTs = millis();
    while (millis() - alertTs < 5000UL) {
      ledRGBUpdate();
      buzzerUpdate();
      yield();
    }
    // Nota: homeCarousel() viene chiamata lo stesso per posizionare il carosello
    // ma finecorsaError bloccher?? le erogazioni successive
    addLog("[BOOT] Procedo con home nonostante errore cremagliera");
  }
}

void homeCarousel() {
  if (!HARDWARE_PRESENT) { addLog("[HOME] Skip (no hardware)"); return; }
  addLog("[HOME] Ricerca...");
  int prevLedMode = ledMode;
  ledMode = 10; // rosso lampeggio veloce durante rotazione
  enableNEMA(true);
  for (int i=0; i<STEPS_PER_SLOT*NUM_SLOTS*4; i++) {
    if (digitalRead(HOME_PIN) == LOW) break;
    digitalWrite(DIR_PIN, LOW);
    digitalWrite(STEP_PIN, HIGH); delayMicroseconds(STEP_DELAY_US);
    digitalWrite(STEP_PIN, LOW);  delayMicroseconds(STEP_DELAY_US);
    if (i%50==0) { ledRGBUpdate(); yield(); }
  }
  currentSlot=1; enableNEMA(false);
  // Ripristina ledMode precedente (o standby se era rotazione)
  ledMode = (prevLedMode == 10) ? 1 : prevLedMode;
  addLog("[HOME] Scomparto 1");
}

void gotoSlot(int t) {
  if (!HARDWARE_PRESENT) { currentSlot=t; return; }
  if (t == 99) return;
  if (t==currentSlot) return;
  int diff=t-currentSlot; if (diff<0) diff+=NUM_SLOTS;
  int prevLedMode = ledMode;
  ledMode = 10; // rosso lampeggio veloce durante rotazione carosello
  enableNEMA(true);
  // stepNEMA con ledRGBUpdate ogni 50 passi
  digitalWrite(DIR_PIN, false ? HIGH : LOW);
  int steps = diff * STEPS_PER_SLOT;
  for (int i=0; i<steps; i++) {
    digitalWrite(STEP_PIN, HIGH); delayMicroseconds(STEP_DELAY_US);
    digitalWrite(STEP_PIN, LOW);  delayMicroseconds(STEP_DELAY_US);
    if (i%50==0) { ledRGBUpdate(); yield(); }
  }
  enableNEMA(false);
  delay(SETTLE_MS);
  currentSlot=t;
  // Se in erogazione (busy): passa subito a blu lampeggiante (attesa conferma)
  // Se non in erogazione: ripristina il modo precedente
  if (busy) {
    ledMode = 3; // blu lampeggio  fine rotazione, sta per erogare
  } else {
    ledMode = (prevLedMode == 10) ? 1 : prevLedMode;
  }
  // Forza aggiornamento LED fisico immediato
  if (ledMode == 3) {
    ledcWrite(LED_R_PIN, 0);
    ledcWrite(LED_G_PIN, 0);
    ledcWrite(LED_B_PIN, 255);
  }
  addLog("[SLOT] " + String(t) + " ledMode=" + String(ledMode) + " busy=" + String(busy));
}

void motorCC(bool fwd, int spd) {
  digitalWrite(MOTOR_IN1, fwd ? HIGH : LOW);
  digitalWrite(MOTOR_IN2, fwd ? LOW : HIGH);
  analogWrite(MOTOR_ENA, spd);
}

void stopMotorCC() {
  digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, HIGH);
  analogWrite(MOTOR_ENA, 0);
}

// Rientro cremagliera con fine corsa GPIO34
// Attende LOW (cremagliera rientrata) con timeout 5 secondi
// Restituisce true=ok, false=timeout
bool retractWithEndstop() {
  #define RETRACT_TIMEOUT_MS 5000
  int savedLedMode = ledMode;
  addLog("[LED] retract: ledMode=" + String(ledMode) + " busy=" + String(busy));
  if (ledMode != 3) ledMode = 10;
  // Forza spegnimento immediato LED rosso e accensione blu se ledMode==3
  if (savedLedMode == 3) {
    ledcWrite(LED_R_PIN, 0);
    ledcWrite(LED_G_PIN, 0);
    ledcWrite(LED_B_PIN, 255); // blu acceso subito
  } else {
    ledcWrite(LED_R_PIN, 255); // rosso acceso subito
    ledcWrite(LED_G_PIN, 0);
    ledcWrite(LED_B_PIN, 0);
  }
  motorCC(false, DISPENSE_SPEED); // motore indietro
  unsigned long tStart = millis();
  bool bzState = true; unsigned long bzLast = millis();
  digitalWrite(BUZZER_PIN, HIGH); // inizia bip
  while (millis() - tStart < RETRACT_TIMEOUT_MS) {
    if (digitalRead(FINECORSA_PIN) == LOW) {
      // Fine corsa scattato  cremagliera rientrata
      stopMotorCC();
      digitalWrite(BUZZER_PIN, LOW); // stop bip
      ledMode = savedLedMode;
      addLog("[DISP] Fine corsa OK (" + String(millis()-tStart) + "ms)");
      return true;
    }
    // Bip veloci 50ms on / 50ms off durante rientro
    unsigned long tNow = millis();
    if (bzState  && tNow - bzLast >= 150) { digitalWrite(BUZZER_PIN, LOW);  bzState=false; bzLast=tNow; }
    if (!bzState && tNow - bzLast >= 150) { digitalWrite(BUZZER_PIN, HIGH); bzState=true;  bzLast=tNow; }
    ledRGBUpdate();
    yield();
    delay(5);
  }
  digitalWrite(BUZZER_PIN, LOW); // sicurezza  spegni buzzer
  // Timeout  forza stop
  stopMotorCC();
  addLog("[DISP] ERRORE: fine corsa non scattato entro 5s!");
  return false;
}

void dispense(int qty) {
  if (!HARDWARE_PRESENT) { addLog("[DISP] Skip (no hardware)"); return; }
  addLog("[DISP] x" + String(qty));
  for (int i=0; i<qty; i++) {
    // Verifica blocco da errore precedente
    if (finecorsaError) {
      addLog("[DISP] BLOCCATO: errore fine corsa precedente  risolvi e riavvia");
      return;
    }
    // Avanzata  bip 150ms on / 150ms off dall'inizio
    motorCC(true, DISPENSE_SPEED);
    { unsigned long tAdv = millis();
      unsigned long bzLast = tAdv; bool bzOn = true;
      digitalWrite(BUZZER_PIN, HIGH); // parte subito
      while (millis() - tAdv < (unsigned long)DISPENSE_MS) {
        unsigned long tNow = millis();
        if (bzOn  && tNow - bzLast >= 150) { digitalWrite(BUZZER_PIN, LOW);  bzOn=false; bzLast=tNow; }
        if (!bzOn && tNow - bzLast >= 150) { digitalWrite(BUZZER_PIN, HIGH); bzOn=true;  bzLast=tNow; }
        ledRGBUpdate();
        yield();
      }
      digitalWrite(BUZZER_PIN, LOW);
    }
    stopMotorCC();
    delay(RETRACT_MS);
    // Rientro con fine corsa
    bool ok = retractWithEndstop();
    if (!ok) {
      // Timeout rientro cremagliera
      finecorsaError = true;
      ledMode = 9; // rosso lampeggio veloce  nuovo modo allarme
      addLog("[DISP] ALLARME: cremagliera non rientrata  erogazioni bloccate");
      // Alert Telegram
      String msg = " ALLARME MedDispenser: cremagliera NON rientrata!";
      msg += " Verificare motore erogatore. Erogazioni bloccate fino al riavvio.";
      tgSendMsg(TG_CHAT_ID, msg, false);
      if (strlen(TG_CHAT_ID2) > 0) tgSendMsg(TG_CHAT_ID2, msg, false);
      buzzerStart(10, 300, 200);
      return;
    }
    if (i<qty-1) delay(DISPENSE_PAUSE);
  }
  addLog("[DISP] OK");
}

//  RIEPILOGO MATTUTINO 
// ?? OTA: controlla aggiornamento firmware ??
void checkOTA() {
  // Controlla una volta al giorno all'orario configurato
  if (getHour() != otaHour || getMin() != otaMin) return;
  long today = timeClient.getEpochTime() / 86400L;
  if (otaLastDay == today) return;
  otaLastDay = today;

  addLog("[OTA] Controllo aggiornamento firmware...");
  WiFiClientSecure client;
  client.setInsecure(); // accetta certificati senza verifica (github raw)
  HTTPClient http;
  http.begin(client, OTA_VERSION_URL);
  int code = http.GET();
  if (code != 200) {
    addLog("[OTA] Errore HTTP: " + String(code));
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();

  // Estrai versione e URL dal JSON
  String remoteVer = "";
  String binUrl = "";
  int vi = payload.indexOf("\"version\"");
  if (vi >= 0) {
    int qs = payload.indexOf("\"", vi+10);
    int qe = payload.indexOf("\"", qs+1);
    if (qs >= 0 && qe > qs) remoteVer = payload.substring(qs+1, qe);
  }
  int ui = payload.indexOf("\"url\"");
  if (ui >= 0) {
    int qs = payload.indexOf("\"", ui+6);
    int qe = payload.indexOf("\"", qs+1);
    if (qs >= 0 && qe > qs) binUrl = payload.substring(qs+1, qe);
  }

  addLog("[OTA] Locale: " FW_VERSION " | Remota: " + remoteVer);

  if (remoteVer.length() == 0 || remoteVer == FW_VERSION) {
    addLog("[OTA] Firmware aggiornato, nessun aggiornamento necessario");
    return;
  }

  // Versione diversa - aggiorna
  addLog("[OTA] Aggiornamento a " + remoteVer + " in corso...");
  tgSendMsg(TG_CHAT_ID, "MedDispenser " + String(deviceId) + ": aggiornamento firmware " + remoteVer + " in corso...", false);
  delay(3000); // aspetta che Telegram parta
  // Salva versione attesa su Firebase per notifica post-riavvio
  fbPut("/config/settings/otaLastVer", """ + remoteVer + """);
  delay(500);
  ledMode = 2;

  WiFiClientSecure updateClient;
  updateClient.setInsecure();

  // Callback chiamato prima del riavvio - invia Telegram di successo
  t_httpUpdate_return ret = httpUpdate.update(updateClient, binUrl);
  switch(ret) {
    case HTTP_UPDATE_FAILED:
      addLog("[OTA] ERRORE: " + httpUpdate.getLastErrorString());
      tgSendMsg(TG_CHAT_ID, "MedDispenser " + String(deviceId) + ": errore OTA: " + httpUpdate.getLastErrorString(), false);
      delay(2000);
      break;
    case HTTP_UPDATE_NO_UPDATES:
      addLog("[OTA] Nessun aggiornamento disponibile");
      break;
    case HTTP_UPDATE_OK:
      // httpUpdate riavvia automaticamente - il messaggio va prima del riavvio
      // Il riavvio avviene DOPO l'uscita da update(), quindi questo non viene eseguito
      // Il messaggio di conferma viene inviato al prossimo avvio tramite pushHB
      break;
  }
  ledMode = 1;
}

void sendMorningSummary() {
  // Controlla summaryForce dall'app
  bool forced = false;
  String forceVal = fbGet(devPath + "/config/settings/summaryForce.json");
  if (forceVal == "true") {
    forced = true;
    fbPut(devPath + "/config/settings/summaryForce", "false");
    addLog("[SUMMARY] Invio forzato dall'app");
  }
  if (!forced) {
    if (getHour() != summaryHour || getMin() != summaryMin) return;
    if (lastSummaryDay == getDay()) return;
  }
  lastSummaryDay = getDay();
  int dow = getDow();
  String dest = String(summaryDest);

  // Raccoglie pazienti unici attivi oggi
  String pazientiList[MAX_SCHEDULES];
  int pazientiCount = 0;
  for (int i=0; i<scheduleCount; i++) {
    Schedule& s = schedules[i];
    if (!s.valid || !s.active) continue;
    if (!isSchedActiveToday(s)) continue;
    bool fire = false;
    if (s.freq == 0) { fire = true; }
    else if (s.freq == 2 || s.freq == 3) {
      int interval = (s.freq == 3) ? 15 : 2;
      long startDays = daysFromStart(s.startDate);
      if (startDays < 0) { fire = true; }
      else {
        long diff = (long)(timeClient.getEpochTime() / 86400L) - startDays;
        fire = (diff >= 0) && (diff % interval == 0);
      }
    } else { fire = ((s.days>>dow)&1); }
    if (!fire) continue;
    String pz = strlen(s.paziente) > 0 ? String(s.paziente) : "Generale";
    bool found = false;
    for (int j=0; j<pazientiCount; j++) if (pazientiList[j] == pz) { found=true; break; }
    if (!found && pazientiCount < MAX_SCHEDULES) pazientiList[pazientiCount++] = pz;
  }
  if (pazientiCount == 0) return;

  struct MsgLine { int minOra; String testo; };
  MsgLine lines[MAX_SCHEDULES];
  int lineCount = 0;

  // Invia un messaggio per ogni paziente
  for (int p=0; p<pazientiCount; p++) {
    String paz = pazientiList[p];
    lineCount = 0;
    int cnt = 0;
    String msg = "Buongiorno! Terapia di oggi";
    if (paz != "Generale") { msg += " per "; msg += paz; }
    msg += ":"; msg += char(13); msg += char(10); msg += char(13); msg += char(10);
    for (int i=0; i<scheduleCount; i++) {
      Schedule& s = schedules[i];
      if (!s.valid || !s.active) continue;
      if (!isSchedActiveToday(s)) continue;
      bool fire = false;
      if (s.freq == 0) {
        fire = true;
      } else if (s.freq == 2 || s.freq == 3) {
        int interval = (s.freq == 3) ? 15 : 2;
        long startDays = daysFromStart(s.startDate);
        if (startDays < 0) { fire = true; }
        else {
          long diff = (long)(timeClient.getEpochTime() / 86400L) - startDays;
          fire = (diff >= 0) && (diff % interval == 0);
        }
      } else { fire = ((s.days>>dow)&1); }
      if (!fire) continue;
      String sPaz = strlen(s.paziente) > 0 ? String(s.paziente) : "Generale";
      if (sPaz != paz) continue;
      String schedJson = fbGet(devPath + "/schedules/" + String(s.id) + "/active.json");
      if (schedJson == "false") continue;
      // Nota frequenza
      String nota = "";
      if (s.freq == 0) { nota = "ogni giorno"; }
      else if (s.freq == 2) { nota = "oggi si - alt."; }
      else if (s.freq == 3) { nota = "oggi si - 15gg"; }
      else {
        const char* nomi[] = {"Dom","Lun","Mar","Mer","Gio","Ven","Sab"};
        String giorni = "";
        for (int d=0; d<7; d++) {
          if ((s.days >> d) & 1) { if (giorni.length()>0) giorni+="/"; giorni+=nomi[d]; }
        }
        nota = giorni;
      }
      String riga = (s.external || s.slot==99) ? "- Esterno: " : "- ";
      riga += String(s.medName);
      riga += " x"; riga += String(s.qty);
      riga += " ore ";
      if (s.hour<10) riga += "0"; riga += String(s.hour);
      riga += ":";
      if (s.minute<10) riga += "0"; riga += String(s.minute);
      if (nota.length() > 0) { riga += "  ["; riga += nota; riga += "]"; }
      if (lineCount < MAX_SCHEDULES) {
        lines[lineCount].minOra = s.hour * 60 + s.minute;
        lines[lineCount].testo  = riga;
        lineCount++;
      }
      cnt++;
    }
    // Ordina per orario (bubble sort)
    for (int a=0; a<lineCount-1; a++) {
      for (int b=0; b<lineCount-1-a; b++) {
        if (lines[b].minOra > lines[b+1].minOra) {
          MsgLine tmp = lines[b]; lines[b] = lines[b+1]; lines[b+1] = tmp;
        }
      }
    }
    for (int i=0; i<lineCount; i++) {
      msg += lines[i].testo;
      msg += char(13); msg += char(10);
    }
    lineCount = 0; // reset per prossimo paziente
    // cancella finto } per chiudere il loop

    if (cnt == 0) continue;
    msg += char(13); msg += char(10);
    msg += "Totale: "; msg += String(cnt); msg += " medicine.";
    if (systemPaused) { msg += char(13); msg += char(10); msg += "ATTENZIONE: Sistema in PAUSA!"; }
    if (dest == "ciro" || dest == "both") tgSendMsg(TG_CHAT_ID, msg, false);
    if (dest == "assunta" || dest == "both") {
      if (strlen(TG_CHAT_ID2) > 0) { delay(1000); tgSendMsg(TG_CHAT_ID2, msg, false); }
    }
    addLog("[TG] Riepilogo " + paz + ": " + String(cnt) + " medicine");
    delay(500);
  }
}

//  SCHEDULER 
int findAlternativeSlot(const char* medName, int excludeSlot) {
  addLog("[FAILOVER] Cerco alternativo per: " + String(medName));
  String targetLow = String(medName); targetLow.toLowerCase();
  // Una sola chiamata HTTP per tutti gli slot (come loadSlotCache)
  String allSlots = fbGet(devPath + "/slots.json");
  if (allSlots == "null" || allSlots.length() < 3) {
    addLog("[FAILOVER] Nessun dato slots");
    return -1;
  }
  for (int s=1; s<=NUM_SLOTS; s++) {
    if (s == excludeSlot) continue;
    // Estrae sotto-oggetto slot_N dal JSON globale
    String key = "\"slot_" + String(s) + "\":" ;
    int ki = allSlots.indexOf(key);
    if (ki == -1) continue;
    int ob = allSlots.indexOf('{', ki);
    if (ob == -1) continue;
    int dep=0, oe=ob;
    for (int i=ob; i<(int)allSlots.length() && i<ob+200; i++) {
      if (allSlots[i]=='{') dep++;
      else if (allSlots[i]=='}') { dep--; if (!dep) { oe=i; break; } }
    }
    String slotObj = allSlots.substring(ob, oe+1);
    String slotMed = parseField(slotObj, "medName");
    String slotQty = parseField(slotObj, "qty");
    if (slotMed.length() == 0 || slotQty.length() == 0) continue;
    String slotMedLow = slotMed; slotMedLow.toLowerCase();
    addLog("[FAILOVER] S" + String(s) + "=" + slotMed + " qty=" + slotQty);
    if (slotMedLow == targetLow && slotQty.toInt() > 0) {
      addLog("[FAILOVER] Trovato scomparto " + String(s));
      return s;
    }
  }
  addLog("[FAILOVER] Nessun alternativo");
  return -1;
}

// Resetta lo stato del BTN2 dopo operazioni bloccanti.
// Accede direttamente alle variabili globali  nessun extern, nessuna static.
void resetBtn2State() {
  btn2WasHigh    = (digitalRead(BTN2_PIN) == HIGH);
  btn2PressStart = 0;
  btn2LongFired  = false;
  btn2ResetNeeded = false;
  addLog("[BTN2] Stato resettato dopo operazione bloccante");
}

void checkCover() {
  if (!HARDWARE_PRESENT) { coverOpen = false; return; }
  bool r1 = digitalRead(COVER_PIN);
  delay(5);
  bool r2 = digitalRead(COVER_PIN);
  delay(5);
  bool r3 = digitalRead(COVER_PIN);
  if (r1 != r2 || r2 != r3) return;
  bool isOpen = (r1 == HIGH);
  if (isOpen && !coverOpen) {
    coverOpen = true;
    loadingMode = true;
    loadingSlot = 0;
    addLog("[COVER] Coperchio APERTO - modalita caricamento");
    ledMode=14;
    displayLoadingReady();
    buzzerStart(2, 200, 100);
  } else if (!isOpen && coverOpen) {
    coverOpen = false;
    loadingMode = false;
    loadingSlot = 0;
    addLog("[COVER] Coperchio CHIUSO - home in corso...");
    delay(800);
    homeCarousel();
    delay(200);
    Wire.begin(21, 22);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    addLog("[COVER] Home completata - sistema operativo");
    ledMode=1;
    resetBtn2State();  // resetta BTN2 dopo home bloccante
  }
}

// Mostra allarme scomparto vuoto sul display per durationMs
// Il testo lampeggia alternando "VUOTO! S<N>" con il display normale ogni 600ms
void displayVuotoAlarm(int slot, const char* medName, int durationMs) {
  if (!displayOk) return;
  unsigned long tStart = millis();
  bool showAlarm = true;
  unsigned long lastSwitch = millis();
  while (millis() - tStart < (unsigned long)durationMs) {
    ledRGBUpdate();
    buzzerUpdate();
    // Alterna display ogni 600ms
    if (millis() - lastSwitch >= 600) {
      lastSwitch = millis();
      showAlarm = !showAlarm;
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      if (showAlarm) {
        // Schermata allarme
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.println("!! VUOTO !!");
        display.setTextSize(1);
        display.setCursor(0, 28);
        display.print("Scomp. S");
        display.print(slot);
        display.print(": ");
        char nm[10]; strncpy(nm, medName, 9); nm[9]=0;
        display.print(nm);
        display.setCursor(0, 42);
        display.print("Nessun alternativo");
        display.setCursor(0, 54);
        display.print("Ricaricare S");
        display.print(slot);
      } else {
        // Schermata normale (ora + slot corrente)
        display.setTextSize(2);
        display.setCursor(0, 16);
        char timeStr[6];
        sprintf(timeStr, "%02d:%02d", getHour(), getMin());
        display.print(timeStr);
        display.setTextSize(1);
        display.setCursor(0, 40);
        display.print("S"); display.print(slot);
        display.print(" VUOTO - ricarica!");
        display.setCursor(0, 52);
        display.print("MedDispenser " FW_VERSION);
      }
      display.display();
    }
    yield();
    delay(10);
  }
  // Alla fine torna al display normale
  updateDisplay();
}

void displayLoadingSlot(int slot) {
  if (!displayOk) return;
  String medName = "";
  if (slot >= 1 && slot <= NUM_SLOTS && strlen(slotNames[slot]) > 0) {
    medName = String(slotNames[slot]);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Riga 1 (y=0, gialla): intestazione
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("=== CARICAMENTO ===");

  // Riga 2 (y=16, blu): S1    Qt:21 ? stessa dimensione, stessa riga
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print("S"); display.print(String(slot));
  display.setCursor(52, 16);
  display.print("Q:"); display.print(String(loadingQty));

  // Riga 3 (y=36, blu): nome medicina
  display.setTextSize(1);
  String mn = medName.length() > 0 ? medName.substring(0, 18) : "(vuoto)";
  display.setCursor(0, 37);
  display.print(mn);

  // Riga 4 (y=56, blu): istruzioni
  display.setCursor(0, 56);
  display.print("+OK  -B2  HoldOK=OK");

  display.display();
}

// Display selezione manuale medicina
// Legge il nome direttamente da /slots/slot_N Firebase (fonte pi? affidabile)
// Il display resta fisso finch? non si tiene premuto GPIO33 per uscire
void displayManualSelect(int slot) {
  if (!displayOk) return;
  // Usa cache locale - istantanea, no HTTP
  String medName = "";
  if (slot >= 1 && slot <= NUM_SLOTS && strlen(slotNames[slot]) > 0) {
    medName = String(slotNames[slot]);
  }
  // Fallback: cerca negli schedules
  if (medName.length() == 0) {
    for (int i=0; i<scheduleCount; i++) {
      if (schedules[i].slot == slot && schedules[i].valid) {
        medName = String(schedules[i].medName);
        break;
      }
    }
  }
  // Leggi quantit? dalla cache locale (istantanea, no HTTP)
  int slotQty = (slot >= 1 && slot <= NUM_SLOTS) ? slotQtys[slot] : -1;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  // Riga gialla y=0: "PRELIEVO S7      qt10"
  display.setCursor(0, 0);
  String prelievoRow = "PRELIEVO S" + String(slot);
  if (slotQty >= 0) {
    // Pad con spazi per allineare a destra su 21 char (128px / 6px per char)
    String qtStr = "qt" + String(slotQty);
    while ((prelievoRow + " " + qtStr).length() < 21) prelievoRow += " ";
    prelievoRow += qtStr;
  }
  display.print(prelievoRow);
  display.setTextSize(2);
  String mn = medName.length() > 0 ? medName.substring(0, 9) : "(vuoto)";
  display.setCursor(0, 16);
  display.print(mn);
  if (medName.length() > 9) {
    display.setCursor(0, 34);
    display.print(medName.substring(9, 18));
  }
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("OK=EROGA  LONG=ESC");
  display.display();
}

// Inizializza loadingQty con il valore di slotMax da Firebase (o default)
void initLoadingQty() {
  loadingQty = SLOT_MAX > 0 ? SLOT_MAX : 12;
}

void displayLoadingReady() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("== CARICAMENTO ==");
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.print("Premi per");
  display.setCursor(10, 38);
  display.print("HOME N1");
  display.display();
}

void displayExternalMed(const char* medName, const char* paziente, int h, int m) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("!! PROMEMORIA !!");
  // Riga paziente
  if (strlen(paziente) > 0) {
    display.setCursor(0, 10);
    char pazShort[18]; strncpy(pazShort, paziente, 17); pazShort[17]=0;
    display.print(pazShort);
  }
  // Nome medicina
  display.setTextSize(1);
  display.setCursor(0, 22);
  char nameShort[18]; strncpy(nameShort, medName, 17); nameShort[17]=0;
  display.print(nameShort);
  display.setTextSize(1);
  display.setCursor(0, 40);
  char timeStr[6]; sprintf(timeStr,"%02d:%02d",h,m);
  display.print("Ore: "); display.print(timeStr);
  display.setCursor(0, 52);
  display.print("Premi OK (tasto )");
  display.display();
}

//  CACHE SLOT e LISTA MANUALE 
// loadSlotCache(): legge /slots/ da Firebase e popola slotNames[]
// Va chiamata in background (setup + ogni 2 min nel loop).
// buildManualSlotList(): costruisce la lista dai dati gi?? in cache  ISTANTANEA.

// Carica TUTTI gli slot con UNA sola chiamata HTTP /slots.json
// ~300ms invece di 10 chiamate x 300ms = 3 secondi
void loadSlotCache() {
  if (!WiFi.isConnected()) return;
  memset(slotNames, 0, sizeof(slotNames));
  addLog("[SLOT] Caricamento cache (1 chiamata)...");
  String json = fbGet(devPath + "/slots.json");
  if (json == "null" || json.length() < 3) {
    addLog("[SLOT] Nessun dato slots");
    slotCacheReady = true;
    lastSlotCacheMs = millis();
    buildManualSlotList();
    return;
  }
  int found = 0, skipped = 0;
  // Parsa ogni slot_N dal JSON globale
  for (int s = 1; s <= NUM_SLOTS; s++) {
    String key = "\"slot_" + String(s) + "\"";
    int ki = json.indexOf(key);
    if (ki == -1) { skipped++; continue; }
    // Estrae il sotto-oggetto dello slot
    int ob = json.indexOf('{', ki);
    if (ob == -1) { skipped++; continue; }
    int dep = 0, oe = ob;
    for (int i = ob; i < (int)json.length() && i < ob+200; i++) {
      if (json[i]=='{') dep++;
      else if (json[i]=='}') { dep--; if (!dep) { oe=i; break; } }
    }
    String slotObj = json.substring(ob, oe+1);
    String nm = parseField(slotObj, "medName");
    nm.trim();
    if (nm.length() == 0) { skipped++; continue; }
    nm.substring(0, 30).toCharArray(slotNames[s], 31);
    String qv = parseField(slotObj, "qty");
    slotQtys[s] = qv.length() > 0 ? qv.toInt() : 0;
    addLog("[SLOT] S" + String(s) + "=" + nm + " qty=" + String(slotQtys[s]));
    found++;
  }
  slotCacheReady = true;
  lastSlotCacheMs = millis();
  addLog("[SLOT] Cache pronta: " + String(found) + " fisici, " + String(skipped) + " vuoti");
  buildManualSlotList();
}

void buildManualSlotList() {
  // Usa solo la cache locale  nessuna chiamata HTTP, istantanea
  manualSlotCount = 0;
  for (int s = 1; s <= NUM_SLOTS; s++) {
    if (strlen(slotNames[s]) > 0) {
      manualSlotList[manualSlotCount++] = s;
    }
  }
  // Aggiusta indice se fuori range (preserva navigazione attiva)
  if (manualSlotIdx >= manualSlotCount) {
    manualSlotIdx = -1;
    manualSelSlot = 0;
  }
  if (manualSlotIdx >= 0 && manualSelSlot > 0) {
    displayManualSelect(manualSelSlot);
  }
  String slotListStr = "";
  for (int i=0; i<manualSlotCount; i++) {
    if (i>0) slotListStr += ", ";
    slotListStr += "S" + String(manualSlotList[i]) + "=" + String(slotNames[manualSlotList[i]]).substring(0,8);
  }
  if (manualSlotCount == 0)
    addLog("[BTN2] ATTENZIONE: nessuno slot disponibile (cache vuota?)");
  else
    addLog("[BTN2] " + String(manualSlotCount) + " slot prelievo: " + slotListStr);
}

void nightHomeCheck() {
  if (getHour() != nightHomeHour || getMin() != nightHomeMin) return;
  if (lastNightHomeDay == getDay()) return;
  lastNightHomeDay = getDay();
  addLog("[HOME] Home notturna automatica...");
  homeCarousel();
  delay(200);
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  addLog("[HOME] Home notturna completata");
  resetBtn2State();
}

// Controlla se uno schedule ?? attivo oggi (endDate + freq periodica)
bool isSchedActiveToday(Schedule& s) {
  // Controlla data fine
  if (strlen(s.endDate) >= 10) {
    int ey = String(s.endDate).substring(0,4).toInt();
    int em = String(s.endDate).substring(5,7).toInt();
    int ed = String(s.endDate).substring(8,10).toInt();
    // Confronto semplice con data odierna
    int ty = 1970 + (int)(timeClient.getEpochTime() / 31557600UL);
    unsigned long epoch = timeClient.getEpochTime();
    unsigned long d = epoch / 86400L;
    int y2 = 1970;
    while(true){int diy=(y2%4==0&&(y2%100!=0||y2%400==0))?366:365;if(d<(unsigned long)diy)break;d-=diy;y2++;}
    int dm[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(y2%4==0&&(y2%100!=0||y2%400==0))dm[1]=29;
    int m2=1; while(d>=(unsigned long)dm[m2-1]){d-=dm[m2-1];m2++;}
    int dd = d + 1;
    // Scaduta se oggi > endDate
    if (y2 > ey || (y2 == ey && m2 > em) || (y2 == ey && m2 == em && dd > ed)) return false;
  }
  return true; // endDate ok o non impostato
}

// Calcola giorni dall'epoch per confronto periodico
long daysFromStart(const char* dateStr) {
  if (strlen(dateStr) < 10) return -1;
  int yr = String(dateStr).substring(0,4).toInt();
  int mo = String(dateStr).substring(5,7).toInt();
  int dy = String(dateStr).substring(8,10).toInt();
  long days = 0;
  for (int y = 1970; y < yr; y++) {
    days += (y%4==0 && (y%100!=0 || y%400==0)) ? 366 : 365;
  }
  int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (yr%4==0 && (yr%100!=0 || yr%400==0)) dm[1] = 29;
  for (int m = 1; m < mo; m++) days += dm[m-1];
  days += dy - 1;
  return days;
}

// Calcola stringa data odierna "YYYY-MM-DD"
String getTodayString() {
  unsigned long epoch = timeClient.getEpochTime();
  unsigned long d = epoch / 86400L;
  int y = 1970;
  while(true){int diy=(y%4==0&&(y%100!=0||y%400==0))?366:365;if(d<(unsigned long)diy)break;d-=diy;y++;}
  int dm[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(y%4==0&&(y%100!=0||y%400==0))dm[1]=29;
  int mo=1; while(d>=(unsigned long)dm[mo-1]){d-=dm[mo-1];mo++;}
  int dd = d + 1;
  char buf[12];
  sprintf(buf, "%04d-%02d-%02d", y, mo, dd);
  return String(buf);
}

// Segna che la medicina di uno slot ?? stata prelevata manualmente oggi
void markManualTaken(int slot) {
  String today = getTodayString();
  fbPut("/manual_taken/" + today + "/slot_" + String(slot), "true");
  addLog("[MANUAL] S" + String(slot) + " prelevato manualmente il " + today);
}

// Controlla se la medicina di uno slot ?? gi?? stata prelevata manualmente oggi
bool isManualTaken(int slot) {
  String today = getTodayString();
  String val = fbGet("/manual_taken/" + today + "/slot_" + String(slot) + ".json");
  return (val == "true");
}

void runScheduler() {
  int h=getHour(), m=getMin(), dow=getDow();
  for (int i=0; i<scheduleCount; i++) {
    Schedule& s = schedules[i];
    if (!s.valid || !s.active) continue;
    if (s.hour != h || s.minute != m) continue;
    // Controlla data fine terapia
    if (!isSchedActiveToday(s)) {
      addLog("[SCHED] " + String(s.medName) + " scaduto (fine terapia)");
      static char lastExpiredId[20] = "";
      if (strcmp(lastExpiredId, s.id) != 0) {
        strncpy(lastExpiredId, s.id, 19);
        tgSendMsg(TG_CHAT_ID, "Terapia SCADUTA: " + String(s.medName) + "  fine terapia raggiunta.", false);
      }
      continue;
    }
    // Calcola se oggi ?? un giorno di erogazione
    bool fire = false;
    if (s.freq == 0) { fire = true; }
    else if (s.freq == 2 || s.freq == 3) {
      int interval = (s.freq == 3) ? 15 : 2;
      long startDays = daysFromStart(s.startDate);
      if (startDays < 0) { fire = true; }
      else {
        long todayDays = (long)(timeClient.getEpochTime() / 86400L);
        long diff = todayDays - startDays;
        fire = (diff >= 0) && (diff % interval == 0);
      }
    } else { fire = ((s.days>>dow)&1); }
    if (!fire) continue;
    String schedJson = fbGet("/schedules/" + String(s.id) + "/active.json");
    if (schedJson == "false") {
      addLog("[SCHED] " + String(s.medName) + " sospeso - skip");
      continue;
    }
    if (strcmp(lastFiredId, s.id)==0 && lastFiredHour==h && lastFiredMin==m) continue;
    String key = "/triggered/" + String(s.id) + "_" + String(getDay()) + "_" + String(getMon());
    String already = fbGet(key + ".json");
    if (already == "true") continue;
    if (s.external || s.slot == 99) {
      addLog("[SCHED] Promemoria esterno: " + String(s.medName));
      fbPut(key, "true");
      strncpy(lastFiredId, s.id, 19);
      lastFiredHour=h; lastFiredMin=m;
      String tgMsg = String("Promemoria: ") + String(s.medName);
      tgMsg += " ore "; tgMsg += String(h); tgMsg += ":";
      if (m<10) tgMsg += "0"; tgMsg += String(m);
      if (strlen(s.paziente) > 0) { tgMsg += "  "; tgMsg += String(s.paziente); }
      tgMsg += ". Rispondi OK per confermare.";
      tgSendMsg(TG_CHAT_ID, tgMsg, false);
      // Aggiorna variabili display con paziente
      strncpy(currentDispMed, s.medName, 31);
      strncpy(currentDispPaziente, s.paziente, 23);
      externalPending = true;
      displayExternalMed(s.medName, s.paziente, h, m);
      buzzerAlert();
      ledMode=3;
      for (int pi=0; pi<MAX_PENDING; pi++) {
        if (!tgPending[pi].valid) {
          strncpy(tgPending[pi].medName, s.medName, 31);
          strncpy(tgPending[pi].chatId, TG_CHAT_ID, 19);
          tgPending[pi].dispenseTime = timeClient.getEpochTime();
          tgPending[pi].reminderSent = 0;
          tgPending[pi].confirmed = false;
          String lk = logDispense(0, s.qty, true, s.medName, false, s.paziente);
          strncpy(tgPending[pi].logKey, lk.c_str(), 23);
          tgPending[pi].valid = true;
          break;
        }
      }
      continue;
    }
    // Controlla scorte e failover
    String slotKey = "/slots/slot_" + String(s.slot);
    String slotJson = fbGet(slotKey + ".json");
    int currentQty = SLOT_MAX;
    String slotMedName = "";
    if (slotJson != "null" && slotJson.length() > 2) {
      String qtyStr = parseField(slotJson, "qty");
      slotMedName = parseField(slotJson, "medName");
      if (qtyStr.length() > 0) currentQty = qtyStr.toInt();
    }
    if (slotMedName.length() == 0) {
      addLog("[SCHED] Slot " + String(s.slot) + " libero - erogo normalmente");
      currentQty = SLOT_MAX;
    }
    int useSlot = s.slot;
    if (currentQty <= 0) {
      int altSlot = findAlternativeSlot(s.medName, s.slot);
      if (altSlot > 0) {
        useSlot = altSlot;
        tgSendMsg(TG_CHAT_ID, "Scomparto " + String(s.slot) + " vuoto! Erogo da scomparto " + String(altSlot) + " (" + String(s.medName) + ")", false);
        addLog("[FAILOVER] Erogazione da scomparto " + String(altSlot) + " invece di " + String(s.slot));
      } else {
        tgSendMsg(TG_CHAT_ID, "BLOCCATO: scomparto " + String(s.slot) + " (" + String(s.medName) + ") VUOTO! Nessun alternativo.", false);
        addLog("[SCHED] BLOCCATO - scomparto " + String(s.slot) + " vuoto, nessun alternativo");
        logDispense(s.slot, s.qty, false, s.medName, false, s.paziente);
        strncpy(lastFiredId, s.id, 19);
        lastFiredHour=h; lastFiredMin=m;
        fbPut(key, "true");
        ledMode=6;
        buzzerStart(8, 200, 200);
        displayVuotoAlarm(s.slot, s.medName, 30000);
        ledMode=1;
        homeCarousel();
        delay(200); Wire.begin(21, 22); display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
        continue;
      }
    }
    if (coverOpen) {
      addLog("[SCHED] Erogazione saltata - coperchio aperto");
      strncpy(lastFiredId, s.id, 19); lastFiredHour=h; lastFiredMin=m;
      fbPut(key, "true"); continue;
    }
    // Controlla se gi?? prelevata manualmente oggi
    if (isManualTaken(useSlot)) {
      addLog("[SCHED] S" + String(useSlot) + " gia prelevata manualmente  invio promemoria assunzione");
      strncpy(lastFiredId, s.id, 19); lastFiredHour=h; lastFiredMin=m;
      fbPut(key, "true");
      // Promemoria assunzione invece di erogazione
      String msgProm = "Promemoria assunzione: " + String(s.medName);
      msgProm += " ore "; if (h<10) msgProm += "0"; msgProm += String(h) + ":";
      if (m<10) msgProm += "0"; msgProm += String(m);
      if (strlen(s.paziente) > 0) { msgProm += " - "; msgProm += String(s.paziente); }
      msgProm += ". Hai gia prelevato questa medicina. Rispondi OK per confermare assunzione.";
      tgSendMsg(TG_CHAT_ID, msgProm, false);
      // Aggiorna display e LED blu
      strncpy(currentDispMed, s.medName, 31);
      strncpy(currentDispPaziente, s.paziente, 23);
      ledMode = 3;
      buzzerAlert();
      // Aggiunge pending per conferma
      for (int pi=0; pi<MAX_PENDING; pi++) {
        if (!tgPending[pi].valid) {
          strncpy(tgPending[pi].medName, s.medName, 31);
          strncpy(tgPending[pi].chatId, TG_CHAT_ID, 19);
          tgPending[pi].dispenseTime = timeClient.getEpochTime();
          tgPending[pi].reminderSent = 0;
          tgPending[pi].confirmed = false;
          String lk = logDispense(useSlot, s.qty, true, s.medName, false, s.paziente);
          strncpy(tgPending[pi].logKey, lk.c_str(), 23);
          tgPending[pi].valid = true;
          break;
        }
      }
      continue;
    }
    // Controlla pausa per paziente specifico
    if (strlen(s.paziente) > 0) {
      String pazPath = "/config/paused_pazienti/" + String(s.paziente) + ".json";
      String pazPaused = fbGet(pazPath);
      if (pazPaused == "true") {
        addLog("[SCHED] Saltato  paziente in pausa: " + String(s.paziente));
        strncpy(lastFiredId, s.id, 19); lastFiredHour=h; lastFiredMin=m;
        fbPut(key, "true"); continue;
      }
    }
    if (finecorsaError) {
      addLog("[SCHED] BLOCCATO: errore fine corsa attivo  erogazione " + String(s.medName) + " saltata");
      tgSendMsg(TG_CHAT_ID, "Erogazione " + String(s.medName) + " SALTATA: errore fine corsa. Riavviare dopo verifica.", false);
      strncpy(lastFiredId, s.id, 19); lastFiredHour=h; lastFiredMin=m;
      fbPut(key, "true"); continue;
    }
    addLog("[SCHED] >>> Erogazione: " + String(s.medName) + " ("+String(s.paziente)+") S" + String(useSlot) + " ore " + String(h) + ":" + (m<10?"0":"") + String(m));
    // Aggiorna variabili display
    strncpy(currentDispMed, s.medName, 31);
    strncpy(currentDispPaziente, s.paziente, 23);
    busy=true; ledMode=2; pushHB();
    fbPut(key, "true");
    strncpy(lastFiredId, s.id, 19);
    lastFiredHour=h; lastFiredMin=m;
    gotoSlot(useSlot); // ledMode=3 impostato automaticamente da gotoSlot
    ledRGBUpdate();
    dispense(s.qty);
    String dispLogKey = logDispense(useSlot, s.qty, true, s.medName, false, s.paziente);
    totalDispenses++; partialDispenses++;
    fbPut(devPath + "/counters/total", String(totalDispenses));
    fbPut(devPath + "/counters/partial", String(partialDispenses));
    updateSlotQty(useSlot, s.medName, s.qty);
    String tgMsg = "Erogazione: ";
    tgMsg += s.medName; tgMsg += " x"; tgMsg += String(s.qty);
    tgMsg += " ore "; tgMsg += String(h); tgMsg += ":";
    if (m<10) tgMsg += "0"; tgMsg += String(m);
    if (strlen(s.paziente) > 0) { tgMsg += "  "; tgMsg += String(s.paziente); }
    tgMsg += ". Rispondi OK per confermare.";
    tgSendMsg(TG_CHAT_ID, tgMsg, false);
    // buzzerAlert rimosso  il bip avviene gi?? durante l'erogazione
    for (int pi=0; pi<MAX_PENDING; pi++) {
      if (!tgPending[pi].valid) {
        strncpy(tgPending[pi].medName, s.medName, 31);
        strncpy(tgPending[pi].chatId, TG_CHAT_ID, 19);
        tgPending[pi].dispenseTime = timeClient.getEpochTime();
        tgPending[pi].reminderSent = 0;
        tgPending[pi].confirmed = false;
        strncpy(tgPending[pi].logKey, dispLogKey.c_str(), 23);
        tgPending[pi].valid = true;
        break;
      }
    }
    ledMode=3;
    // reset display solo dopo conferma
    busy=false; pushHB();
  }
}

// Controlla notifiche agenda su Firebase e le invia via Telegram + CallMeBot
void checkAgendaNotify() {
  if (!WiFi.isConnected()) return;
  // Legge notifica Telegram
  String tgNotify = fbGet(devPath + "/agenda_notify.json");
  if (tgNotify != "null" && tgNotify.length() > 10) {
    String msg = parseField(tgNotify, "msg");
    if (msg.length() > 0) {
      addLog("[AGENDA] Notifica TG: " + msg.substring(0,40));
      tgSendMsg(TG_CHAT_ID, msg, false);
      if (strlen(TG_CHAT_ID2) > 0) {
        delay(500);
        tgSendMsg(TG_CHAT_ID2, msg, false);
      }
      // Cancella la notifica dopo averla inviata
      fbPut(devPath + "/agenda_notify", "null");
    }
  }
  // Legge notifica WhatsApp/chiamata
  String waNotify = fbGet(devPath + "/agenda_wa_notify.json");
  if (waNotify != "null" && waNotify.length() > 10) {
    String msgWa = parseField(waNotify, "msg");
    if (msgWa.length() > 0) {
      addLog("[AGENDA] Notifica WA: " + msgWa.substring(0,30));
      waCall(msgWa.c_str());
      fbPut(devPath + "/agenda_wa_notify", "null");
    }
  }
}

void loadSettings() {
  String json = fbGet(devPath + "/config/settings.json");
  if (json == "null" || json.length() < 3) return;
  String ssid    = parseField(json, "ssid");
  String pass    = parseField(json, "wifipass");
  String tg1     = parseField(json, "tg1");
  String tg2     = parseField(json, "tg2");
  String remind  = parseField(json, "remind");
  String alert   = parseField(json, "alert");
  String waphone = parseField(json, "waphone");
  String wakey   = parseField(json, "wakey");
  if (ssid.length()>0)    { ssid.toCharArray(WIFI_SSID, 64); }
  if (pass.length()>0)    { pass.toCharArray(WIFI_PASS, 64); }
  if (tg1.length()>0)     { tg1.toCharArray(TG_CHAT_ID, 24); }
  if (tg2.length()>0)     { tg2.toCharArray(TG_CHAT_ID2, 24); }
  if (remind.length()>0)  { TG_REMIND_MIN = remind.toInt(); }
  if (alert.length()>0)   { TG_ALERT_MIN  = alert.toInt(); }
  if (waphone.length()>0) {
    String ph = waphone;
    ph.replace("+", "%2B");
    ph.toCharArray(WA_PHONE, 32);
  }
  if (wakey.length()>0)   { wakey.toCharArray(WA_APIKEY, 16); }
  String cJson = fbGet(devPath + "/counters.json");
  if (cJson != "null" && cJson.length() > 2) {
    String tot = parseField(cJson, "total");
    String par = parseField(cJson, "partial");
    if (tot.length() > 0) totalDispenses = tot.toInt();
    if (par.length() > 0) partialDispenses = par.toInt();
    addLog("[CNT] Totale=" + String(totalDispenses) + " Parziale=" + String(partialDispenses));
  }
  addLog("[CFG] Impostazioni caricate da Firebase");
  String mJson = fbGet(devPath + "/config/messages.json");
  if (mJson != "null" && mJson.length() > 2) {
    String sh = parseField(mJson, "summaryHour");
    String sm = parseField(mJson, "summaryMin");
    if (sh.length() > 0) summaryHour = sh.toInt();
    if (sm.length() > 0) summaryMin  = sm.toInt();
    String oth = parseField(mJson, "otaHour");
    String otm = parseField(mJson, "otaMin");
    if (oth.length() > 0) otaHour = oth.toInt();
    if (otm.length() > 0) otaMin  = otm.toInt();
    String sd = parseField(mJson, "summaryDest");
    if (sd.length() > 0) sd.toCharArray(summaryDest, 10);
    String rm = parseField(mJson, "remindMin");
    String am = parseField(mJson, "alertMin");
    if (rm.length() > 0) TG_REMIND_MIN = rm.toInt();
    if (am.length() > 0) TG_ALERT_MIN  = am.toInt();
    addLog("[CFG] Riepilogo alle " + String(summaryHour) + ":" + (summaryMin<10?"0":"") + String(summaryMin) + " a " + String(summaryDest));
    String nhh = parseField(mJson, "nightHomeHour");
    String nhm = parseField(mJson, "nightHomeMin");
    if (nhh.length() > 0) nightHomeHour = nhh.toInt();
    if (nhm.length() > 0) nightHomeMin  = nhm.toInt();
    addLog("[HOME] Home notturna alle " + String(nightHomeHour) + ":" + (nightHomeMin<10?"0":"") + String(nightHomeMin));
  }
}

void saveMotorToFlash() {
  prefs.begin("motor", false);
  prefs.putInt("steps", STEPS_PER_SLOT);
  prefs.putInt("delay", STEP_DELAY_US);
  prefs.putInt("dispms", DISPENSE_MS);
  prefs.putInt("dispspd", DISPENSE_SPEED);
  prefs.putInt("settlems", SETTLE_MS);
  prefs.end();
  addLog("[MOTOR] Parametri salvati in flash");
}

void loadMotorFromFlash() {
  prefs.begin("motor", true);
  int s = prefs.getInt("steps", 0);
  int d = prefs.getInt("delay", 0);
  int ms = prefs.getInt("dispms", 0);
  int spd = prefs.getInt("dispspd", 0);
  int settle = prefs.getInt("settlems", 0);
  prefs.end();
  if (s > 0) {
    STEPS_PER_SLOT = s; STEP_DELAY_US = d; DISPENSE_MS = ms; DISPENSE_SPEED = spd;
    if (settle > 0) SETTLE_MS = settle;
    addLog("[MOTOR] Parametri da flash: steps=" + String(s) + " delay=" + String(d));
  }
}

void loadMotorConfig() {
  String json = fbGet(devPath + "/config/motor.json");
  if (json == "null" || json.length() < 3) return;
  String steps   = parseField(json, "steps_per_slot");
  String stepDel = parseField(json, "step_delay_us");
  String dispMs  = parseField(json, "dispense_ms");
  String dispSpd = parseField(json, "dispense_speed");
  String settlMs = parseField(json, "settle_ms");
  if (steps.length()>0)   STEPS_PER_SLOT = steps.toInt();
  if (stepDel.length()>0) STEP_DELAY_US  = stepDel.toInt();
  if (dispMs.length()>0)  DISPENSE_MS    = dispMs.toInt();
  if (dispSpd.length()>0) DISPENSE_SPEED = dispSpd.toInt();
  if (settlMs.length()>0) SETTLE_MS      = settlMs.toInt();
  addLog("[CFG] Steps=" + String(STEPS_PER_SLOT) + " Delay=" + String(STEP_DELAY_US) +
         " DispMS=" + String(DISPENSE_MS) + " DispSpd=" + String(DISPENSE_SPEED) + " Settle=" + String(SETTLE_MS) + "ms");
  saveMotorToFlash();
  // Carica capacit?? massima scomparto
  String smJson = fbGet(devPath + "/config/slot_max.json");
  if (smJson != "null" && smJson.length() > 0) {
    int sm = smJson.toInt();
    if (sm >= 1 && sm <= 100) { SLOT_MAX = sm; addLog("[CFG] SlotMax=" + String(SLOT_MAX)); }
  }
}

void loadSchedules() {
  addLog("[SCHED] Caricamento...");
  String json = fbGet(devPath + "/schedules.json");
  scheduleCount = 0;
  if (json == "null" || json.length() < 3) { addLog("[SCHED] Nessun orario"); return; }
  int pos = 0;
  while (scheduleCount < MAX_SCHEDULES) {
    int ap = json.indexOf("\"active\":true", pos);
    if (ap == -1) break;
    int os = json.lastIndexOf('{', ap);
    if (os == -1) { pos=ap+1; continue; }
    int dep=0, oe=os;
    for (int i=os; i<(int)json.length() && i<os+500; i++) {
      if (json[i]=='{') dep++;
      else if (json[i]=='}') { dep--; if (!dep) { oe=i; break; } }
    }
    pos = oe + 1;
    if (oe <= os) continue;
    String obj = json.substring(os, oe+1);
    if (obj.length() > 450) continue;
    String ts = parseField(obj, "time");
    if (ts.length() < 5 || ts[2] != ':') continue;
    int hh=ts.substring(0,2).toInt(), mm=ts.substring(3,5).toInt();
    if (hh<0||hh>23||mm<0||mm>59) continue;
    Schedule& s = schedules[scheduleCount];
    memset(&s, 0, sizeof(Schedule));
    s.valid=s.active=true; s.hour=hh; s.minute=mm;
    s.slot = parseField(obj,"slot").toInt();
    if (s.slot != 99 && (s.slot<1||s.slot>NUM_SLOTS)) s.slot=1;
    s.qty = (uint8_t)parseField(obj,"qty").toFloat();
    if (s.qty<1||s.qty>10) s.qty=1;
    String fr = parseField(obj,"freq");
    if (fr.indexOf("every15")!=-1)     s.freq = 3; // ogni 15 giorni
    else if (fr.indexOf("altdays")!=-1) s.freq = 2; // giorni alterni
    else if (fr.indexOf("daily")!=-1)   s.freq = 0; // ogni giorno
    else                                 s.freq = 1; // settimanale/custom
    // Carica startDate per altdays
    String sd = parseField(obj,"startDate");
    sd.substring(0,10).toCharArray(s.startDate,12);
    s.days = 0b1111111;
    int ds = obj.indexOf("\"days\":[");
    if (ds != -1) {
      s.days=0;
      int de=obj.indexOf(']',ds);
      if (de>ds) {
        String da=obj.substring(ds+8,de);
        for (int d=0;d<=6;d++) if (da.indexOf(String(d))!=-1) s.days|=(1<<d);
      }
    }
    String mn=parseField(obj,"medName");
    int ci=0;
    for (int i=0;i<(int)mn.length()&&ci<30;i++) {
      if ((unsigned char)mn[i]>=32&&mn[i]<127) s.medName[ci++]=mn[i];
    }
    s.medName[ci]=0;
    parseField(obj,"id").substring(0,18).toCharArray(s.id,20);
    // Carica paziente
    String pz = parseField(obj,"paziente");
    pz.substring(0,22).toCharArray(s.paziente,24);
    // Carica date inizio/fine
    parseField(obj,"endDate").substring(0,10).toCharArray(s.endDate,12);
    String tp = parseField(obj,"type");
    s.external = (tp == "external") || (s.slot == 99);
    scheduleCount++;
    if (s.external || s.slot == 99) {
      addLog("[SCHED] [EXT] " + String(s.medName) + " " + String(s.hour) + ":" + (s.minute<10?"0":"") + String(s.minute) + "  farmaco esterno");
    } else {
      addLog("[SCHED] [FIS] " + String(s.medName) + " " + String(s.hour) + ":" + (s.minute<10?"0":"") + String(s.minute) + "  scomparto S" + String(s.slot));
    }
  }
  addLog("[SCHED] Tot: " + String(scheduleCount));
  // La lista slot (manualSlotList) ?? gestita da loadSlotCache()  non toccarla qui.
  // loadSlotCache() viene chiamata al boot e ogni 10 minuti nel loop.
}

void processCommand(String& json) {
  String cmd = parseField(json, "cmd");
  cmd.replace("\"",""); cmd.trim();
  addLog("[CMD] " + cmd);
  if (coverOpen && (cmd=="dispense"||cmd=="goto_slot"||cmd=="rotate_carousel")) {
    addLog("[COVER] Comando " + cmd + " bloccato - coperchio aperto");
    return;
  }
  busy=true; ledMode=2; pushHB();
  loadMotorConfig();
  if (cmd=="dispense") {
    if (finecorsaError) {
      addLog("[CMD] dispense bloccato - errore fine corsa attivo");
      busy=false; pushHB(); return;
    }
    if (millis() - lastAutoDispense < 30000UL) {
      addLog("[CMD] dispense ignorato - erogazione automatica recente");
      busy=false; pushHB(); return;
    }
    int sl=parseField(json,"slot").toInt();
    int qt=(int)parseField(json,"qty").toFloat();
    String mn=parseField(json,"medName");
    if (sl<1||sl>NUM_SLOTS) sl=currentSlot;
    if (qt<1||qt>10) qt=1;
    // Controlla scorte  failover se vuoto (stesso comportamento manuale e automatico)
    int useSlot = sl;
    String slotJson2 = fbGet("/slots/slot_" + String(sl) + ".json");
    int cmdQty = SLOT_MAX;
    if (slotJson2 != "null" && slotJson2.length() > 2) {
      String qtyStr2 = parseField(slotJson2, "qty");
      if (qtyStr2.length() > 0) cmdQty = qtyStr2.toInt();
    }
    if (cmdQty <= 0 && mn.length() > 0) {
      int altSlot = findAlternativeSlot(mn.c_str(), sl);
      if (altSlot > 0) {
        addLog("[CMD] Scomparto " + String(sl) + " vuoto  failover su S" + String(altSlot));
        tgSendMsg(TG_CHAT_ID, "Scomparto " + String(sl) + " vuoto! Erogo da S" + String(altSlot) + " (" + mn + ")", false);
        useSlot = altSlot;
      } else {
        addLog("[CMD] Scomparto " + String(sl) + " vuoto  nessun alternativo");
        tgSendMsg(TG_CHAT_ID, " Scomparto " + String(sl) + " (" + mn + ") VUOTO! Nessun alternativo.", false);
        ledMode=6;
        buzzerStart(5,200,200);
        displayVuotoAlarm(sl, mn.c_str(), 5000);
        ledMode=1; busy=false; pushHB(); return;
      }
    }
    strncpy(currentDispMed, mn.c_str(), 31);
    strncpy(currentDispPaziente, "", 1);
    gotoSlot(useSlot);
    dispense(qt);
    String dispLogKey2 = logDispense(useSlot,qt,true,mn.c_str(),true);
    markManualTaken(useSlot); // segna come prelevato manualmente oggi
    updateSlotQty(useSlot, mn.c_str(), qt);
    String tgMsgCmd = "Erogazione: " + mn + " x" + String(qt) + " da S" + String(useSlot);
    if (useSlot != sl) tgMsgCmd += " (failover da S" + String(sl) + ")";
    tgMsgCmd += ". Rispondi OK per confermare.";
    tgSendMsg(TG_CHAT_ID, tgMsgCmd, false);
    buzzerAlert();
    for (int pi=0; pi<MAX_PENDING; pi++) {
      if (!tgPending[pi].valid) {
        mn.toCharArray(tgPending[pi].medName, 31);
        strncpy(tgPending[pi].chatId, TG_CHAT_ID, 19);
        tgPending[pi].dispenseTime = timeClient.getEpochTime();
        tgPending[pi].reminderSent = 0;
        tgPending[pi].confirmed = false;
        strncpy(tgPending[pi].logKey, dispLogKey2.c_str(), 23);
        tgPending[pi].valid = true;
        break;
      }
    }
    ledMode = 3; // blu attesa conferma
  } else if (cmd=="goto_slot") {
    int sl=parseField(json,"slot").toInt();
    if (sl>=1&&sl<=NUM_SLOTS) gotoSlot(sl);
    ledMode=1; busy=false; pushHB();
  } else if (cmd=="rotate_carousel") {
    int st=parseField(json,"steps").toInt();
    if (!st) st=STEPS_PER_SLOT;
    addLog("[NEMA] Avvio " + String(st) + " passi");
    enableNEMA(true);
    delayMicroseconds(500);
    stepNEMA(st,false);
    enableNEMA(false);
    currentSlot = (currentSlot % NUM_SLOTS) + 1;
    addLog("[NEMA] Scomparto aggiornato: " + String(currentSlot));
    addLog("[NEMA] Completato");
    ledMode=1; busy=false; pushHB();
  } else if (cmd=="home_carousel") {
    homeCarousel();
    ledMode=1; busy=false; pushHB();
  } else if (cmd=="reset_partial") {
    partialDispenses = 0;
    fbPut(devPath + "/counters/partial", "0");
    addLog("[CNT] Contatore parziale azzerato");
  } else if (cmd=="stop_all") {
    stopMotorCC(); enableNEMA(false);
    addLog("[STOP] Fermato");
  } else if (cmd=="reload_schedules") {
    loadSchedules();
  } else if (cmd=="reset_finecorsa") {
    finecorsaError = false;
    ledMode = 1;
    addLog("[CMD] Errore fine corsa resettato manualmente");
    tgSendMsg(TG_CHAT_ID, " Errore fine corsa resettato. Sistema operativo.", false);
  } else if (cmd == "svuota") {
    int sl  = parseField(json, "slot").toInt();
    int qt  = parseField(json, "qty").toInt();
    if (sl < 1 || sl > NUM_SLOTS || qt < 1) {
      addLog("[SVUOTA] Parametri non validi slot=" + String(sl) + " qty=" + String(qt));
    } else {
      addLog("[SVUOTA] Avvio S" + String(sl) + " x" + String(qt));
      busy = true; ledMode = 2; pushHB();
      gotoSlot(sl);
      for (int i = 0; i < qt; i++) {
        addLog("[SVUOTA] " + String(i+1) + "/" + String(qt));
        dispense(1);
        delay(400);
      }
      fbPut(devPath + "/slots/slot_" + String(sl) + "/qty", "0");
      fbPut(devPath + "/command/done", "true");
      tgSendMsg(TG_CHAT_ID, "Scomparto S" + String(sl) + " svuotato: " + String(qt) + " capsule erogate.", false);
      addLog("[SVUOTA] Completato S" + String(sl));
      ledMode = 1; busy = false; pushHB();
      homeCarousel();
    }
  } else if (cmd=="set_pause") {
    String val = parseField(json, "value");
    if (val == "true") {
      systemPaused = true;
      ledMode = 3; // blu lento = in pausa
      addLog("[CMD] Sistema messo in PAUSA");
      fbPut(devPath + "/config/paused", "true");
    } else {
      systemPaused = false;
      ledMode = 1; // verde = attivo
      standbyStart = millis();
      addLog("[CMD] Sistema RIPRESO");
      fbPut(devPath + "/config/paused", "false");
    }
  }
  busy=false; pushHB();
}

//  FIREBASE 
String fbGet(String path) {
  if (!WiFi.isConnected()) return "null";
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  http.begin(c, "https://" + String(FIREBASE_HOST) + path);
  int code=http.GET();
  String p="null"; if (code==200) p=http.getString();
  http.end();
  return p;
}

bool fbPut(String path, String json) {
  if (!WiFi.isConnected()) return false;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  http.begin(c, "https://" + String(FIREBASE_HOST) + path + ".json");
  http.addHeader("Content-Type","application/json");
  int code=http.PUT(json);
  http.end();
  return code==200;
}

void updateSlotQty(int slot, const char* name, int dispensed) {
  if (slot == 99) return;
  String key = "/slots/slot_" + String(slot);
  String json = fbGet(key + ".json");
  if (json == "null" || json.length() < 2) {
    addLog("[SLOT] Slot " + String(slot) + " non trovato  skip");
    return;
  }
  String existingName = parseField(json, "medName");
  if (existingName.length() == 0) {
    addLog("[SLOT] Slot " + String(slot) + " senza medName  skip");
    return;
  }
  String qtyStr = parseField(json, "qty");
  int currentQty = qtyStr.length() > 0 ? qtyStr.toInt() : SLOT_MAX;
  int newQty = max(0, currentQty - dispensed);
  String j = "{\"medName\":\"" + existingName + "\",\"qty\":" + String(newQty) + ",\"max\":" + String(SLOT_MAX) + "}";
  fbPut(key, j);
  addLog("[SLOT] " + existingName + " scorte: " + String(currentQty) + " -> " + String(newQty));
}

String logDispense(int slot, int qty, bool ok, const char* name, bool manual, const char* paziente) {
  unsigned long ts=timeClient.getEpochTime();
  String j="{\"slot\":"+String(slot)+",\"qty\":"+String(qty)+
            ",\"success\":"+(ok?"true":"false")+
            ",\"medName\":\""+String(name)+"\""+
            ",\"paziente\":\""+String(paziente)+"\""+
            ",\"hour\":"+String(getHour())+
            ",\"minute\":"+String(getMin())+
            ",\"day\":"+String(getDay())+
            ",\"month\":"+String(getMon())+
            ",\"confirmed\":false"+
            ",\"manual\":"+(manual?"true":"false")+
            ",\"ts\":"+String(ts)+"}";
  fbPut(devPath + "/log/"+String(ts), j);
  return String(ts);
}

void pushHB() {
  unsigned long epoch=timeClient.getEpochTime();
  String j="{\"state\":\""+ String(finecorsaError?"ALLARME_FINECORSA":systemPaused?"paused":busy?"busy":"ready") +"\""+
            ",\"slot\":"+String(currentSlot)+
            ",\"rssi\":"+String(WiFi.RSSI())+
            ",\"fw_version\":\"" FW_VERSION "\""+
            ",\"scheds\":"+String(scheduleCount)+
            ",\"hour\":"+String(getHour())+
            ",\"min\":"+String(getMin())+
            ",\"ts\":"+String(epoch)+
            ",\"last_heartbeat\":"+String(epoch)+"}";
  fbPut(devPath + "/device_status", j);
}

void wifiPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("MedDispenser-Setup", "12345678");
  IPAddress ip = WiFi.softAPIP();
  addLog("[WiFi] Portale: http://" + ip.toString());

  if (displayOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("CONFIG WiFi");
    display.println("Connettiti a:");
    display.println("MedDispenser-Setup");
    display.println("Password: 12345678");
    display.println("Poi apri:");
    display.println(ip.toString());
    display.display();
  }

  dnsServer.start(53, "*", WiFi.softAPIP());
  WebServer portal(80);
  portal.onNotFound([&](){
    portal.sendHeader("Location", "http://192.168.4.1/", true);
    portal.send(302, "text/plain", "");
  });
  portal.on("/", [&](){
    int n = WiFi.scanNetworks();
    String nets = "";
    for (int i=0; i<n; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      String lock = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "?" : "?";
      String bars = rssi > -50 ? "" : rssi > -65 ? "" : rssi > -75 ? "" : "";
      nets += "<div class='net' onclick='selNet(this)' data-ssid='" + ssid + "'>"
              + lock + " <span class='ssid'>" + ssid + "</span>"
              + "<span class='rssi'>" + bars + "</span></div>";
    }
    String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>"
      "body{font-family:sans-serif;background:#0a0f1e;color:#e2e8f0;padding:16px;max-width:400px;margin:0 auto;}"
      "h2{color:#38bdf8;margin-bottom:16px;font-size:18px;}"
      ".net{padding:12px;margin:6px 0;background:#1e2d42;border-radius:8px;cursor:pointer;display:flex;justify-content:space-between;align-items:center;border:2px solid transparent;}"
      ".net:active,.net.sel{border-color:#38bdf8;background:#1a3a52;}"
      ".ssid{font-size:14px;font-weight:600;}"
      ".rssi{font-size:12px;color:#94a3b8;letter-spacing:1px;}"
      "input{width:100%;padding:10px;margin:6px 0;background:#1e2d42;border:1px solid #333;border-radius:8px;color:#e2e8f0;font-size:14px;box-sizing:border-box;}"
      "label{font-size:12px;color:#94a3b8;display:block;margin-top:12px;}"
      "button{width:100%;padding:12px;background:#38bdf8;color:#0a0f1e;border:none;border-radius:8px;font-size:15px;font-weight:700;cursor:pointer;margin-top:12px;}"
      ".sec{font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:1px;margin:16px 0 8px;}"
      "</style></head>"
      "<body><h2>? MedDispenser Setup</h2>"
      "<div class='sec'>Reti disponibili</div>"
      + nets +
      "<form action='/save' method='post'>"
      "<label>Rete selezionata</label>"
      "<input name='ssid' id='ssid' placeholder='Seleziona rete o scrivi SSID' value='" + String(WIFI_SSID) + "'>"
      "<label>Password</label>"
      "<input name='pass' id='pass' type='text' placeholder='Password WiFi'>"
      "<button type='submit'> Connetti e salva</button>"
      "</form>"
      "<script>function selNet(el){"
      "document.querySelectorAll('.net').forEach(n=>n.classList.remove('sel'));"
      "el.classList.add('sel');"
      "document.getElementById('ssid').value=el.dataset.ssid;"
      "document.getElementById('pass').focus();"
      "}</script>"
      "</body></html>";
    portal.send(200, "text/html", html);
  });

  portal.on("/save", HTTP_POST, [&](){
    String ssid = portal.arg("ssid");
    String pass = portal.arg("pass");
    if (ssid.length() > 0) {
      ssid.toCharArray(WIFI_SSID, 64);
      pass.toCharArray(WIFI_PASS, 64);
      prefs.begin("wifi", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), pass.c_str());
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500); attempts++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        portal.send(200, "text/html",
          "<html><head><meta charset='utf-8'>"
          "<style>body{font-family:sans-serif;background:#0a0f1e;color:#e2e8f0;padding:20px;text-align:center;}</style></head>"
          "<body><h2 style='color:#34d399'> Connesso!</h2><p>IP: " + WiFi.localIP().toString() + "</p><p>Il dispositivo si riavvier?? ora.</p></body></html>");
        delay(3000);
      } else {
        portal.send(200, "text/html",
          "<html><head><meta charset='utf-8'>"
          "<style>body{font-family:sans-serif;background:#0a0f1e;color:#e2e8f0;padding:20px;text-align:center;}</style></head>"
          "<body><h2 style='color:#f87171'> Connessione fallita</h2><p>Verifica SSID e password e riprova.</p><a href='/' style='color:#38bdf8'> Riprova</a></body></html>");
        delay(3000);
        WiFi.mode(WIFI_AP);
        WiFi.softAP("MedDispenser-Setup", "12345678");
        return;
      }
      ESP.restart();
    } else {
      portal.send(400, "text/html", "<html><body>SSID non valido</body></html>");
    }
  });

  portal.begin();
  addLog("[WiFi] Portale attivo  in attesa configurazione...");
  unsigned long start = millis();
  while (millis() - start < 180000UL) {
    dnsServer.processNextRequest();
    portal.handleClient();
    delay(10);
  }
  addLog("[WiFi] Timeout portale  riavvio");
  ESP.restart();
}

bool wifiConnect() {
  prefs.begin("wifi", true);
  String savedSsid = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();
  if (savedSsid.length() > 0) {
    savedSsid.toCharArray(WIFI_SSID, 64);
    savedPass.toCharArray(WIFI_PASS, 64);
    addLog("[WiFi] Credenziali da flash: " + savedSsid);
  } else {
    addLog("[WiFi] Nessuna credenziale salvata - avvio portale");
  }

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(COVER_PIN, INPUT_PULLUP);
  pinMode(FINECORSA_PIN, INPUT_PULLUP); // GPIO15  pullup interno, NO tra pin e GND
  delay(100);
  coverOpen = HARDWARE_PRESENT ? (digitalRead(COVER_PIN) == HIGH) : false;
  if (coverOpen) addLog("[COVER] Boot: coperchio APERTO");
  delay(100);
  if (digitalRead(BTN_PIN) == LOW) {
    addLog("[WiFi] Pulsante premuto - portale forzato");
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    WIFI_SSID[0] = 0;
    WIFI_PASS[0] = 0;
  }

  if (strlen(WIFI_SSID) == 0) {
    addLog("[WiFi] SSID vuoto - avvio portale configurazione");
    wifiPortal();
    return false;
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  addLog("[WiFi] Connessione a: " + String(WIFI_SSID));
  if (displayOk) {
    display.clearDisplay();
    display.setCursor(0,0); display.setTextSize(1);
    display.println("Connessione WiFi...");
    display.display();
  }
  int t=0;
  while (WiFi.status()!=WL_CONNECTED && t<60) {
    delay(500); t++;
    if (t%10==0) addLog("[WiFi] tentativo " + String(t/2) + "s...");
  }
  if (WiFi.status()==WL_CONNECTED) {
    addLog("[WiFi] Connesso a " + String(WIFI_SSID) + " IP: " + WiFi.localIP().toString());
    timeClient.begin();
    int nt=0;
    while (!timeClient.update() && nt<10) { timeClient.forceUpdate(); delay(500); nt++; }
    updateNTPOffset();
    timeClient.update();
    addLog("[NTP] Ora: " + String(getHour()) + ":" + (getMin()<10?"0":"") + String(getMin()) +
           (isDST(getDay(), getMon(), getDow()) ? " (ora legale)" : " (ora solare)"));
    return true;
  }
  addLog("[WiFi] Connessione fallita  avvio portale configurazione");
  wifiPortal();
  return false;
}

//  TEMPO 
int getHour() { return timeClient.getHours();   }
int getMin()  { return timeClient.getMinutes(); }
int getDow()  { return timeClient.getDay();     }

int getDay() {
  unsigned long e=timeClient.getEpochTime();
  if (e<1000000000UL) return 1;
  unsigned long d=e/86400L; int y=1970;
  while(true){int diy=(y%4==0&&(y%100!=0||y%400==0))?366:365;if(d<(unsigned long)diy)break;d-=diy;y++;}
  int dm[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(y%4==0&&(y%100!=0||y%400==0))dm[1]=29;
  int m=1; while(d>=(unsigned long)dm[m-1]){d-=dm[m-1];m++;}
  return d+1;
}

int getMon() {
  unsigned long e=timeClient.getEpochTime();
  if (e<1000000000UL) return 1;
  unsigned long d=e/86400L; int y=1970;
  while(true){int diy=(y%4==0&&(y%100!=0||y%400==0))?366:365;if(d<(unsigned long)diy)break;d-=diy;y++;}
  int dm[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(y%4==0&&(y%100!=0||y%400==0))dm[1]=29;
  int m=1; while(d>=(unsigned long)dm[m-1]){d-=dm[m-1];m++;}
  return m;
}

//  TELEGRAM 
bool tgSendMsg(const char* chatId, String msg, bool withBtn) {
  if (!WiFi.isConnected()) return false;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  String url = "https://api.telegram.org/bot";
  url += TG_TOKEN; url += "/sendMessage";
  http.begin(c, url);
  http.addHeader("Content-Type","application/json");
  String q=String('"');
  String body="{";
  body+=q+"chat_id"+q+":"+q+chatId+q+",";
  body+=q+"text"+q+":"+q+msg+q;
  if (withBtn) {
    body+=","+q+"reply_markup"+q+":{"+q+"inline_keyboard"+q+":[[{";
    body+=q+"text"+q+":"+q+"Confermo"+q+",";
    body+=q+"callback_data"+q+":"+q+"confirm"+q+"}]]}";
  }
  body+="}";
  int code=http.POST(body);
  http.end();
  addLog("[TG] " + String(code));
  return code==200;
}

static long tgOffset = 0;

void tgCheckConfirm() {
  if (!WiFi.isConnected()) return;
  unsigned long nowEpoch = timeClient.getEpochTime();
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  String url = "https://api.telegram.org/bot";
  url += TG_TOKEN;
  url += "/getUpdates?timeout=1&offset=";
  url += String(tgOffset);
  http.begin(c, url);
  int code=http.GET();
  if (code==200) {
    String resp=http.getString();
    if (resp.length()>20) addLog("[TG] Poll OK len=" + String(resp.length()));
    int ui=resp.indexOf("update_id");
    if (ui!=-1) {
      String us=""; int j=ui+11;
      while (j<(int)resp.length()&&resp[j]!=','&&resp[j]!='}') us+=resp[j++];
      us.trim();
      long newUid=us.toInt();
      if (newUid>=tgOffset) tgOffset=newUid+1;
    }
    bool hasConfirm=false;
    if (resp.indexOf("\"text\"")!=-1) {
      hasConfirm=(resp.indexOf("confermo")!=-1)||
                 (resp.indexOf("Confermo")!=-1)||
                 (resp.indexOf("confirm")!=-1)||
                 (resp.indexOf("\"ok\"")!=-1)||
                 (resp.indexOf("\"si\"")!=-1)||
                 (resp.indexOf("\"Si\"")!=-1)||
                 (resp.indexOf("\"OK\"")!=-1);
    }
    if (hasConfirm) {
      addLog("[TG] Conferma trovata!");
      for (int i=0;i<MAX_PENDING;i++) {
        if (tgPending[i].valid&&!tgPending[i].confirmed) {
          tgPending[i].confirmed=true;
          if (tgPending[i].logKey[0] != 0) {
            fbPut(devPath + "/log/" + String(tgPending[i].logKey) + "/confirmed", "true");
            fbPut(devPath + "/log/" + String(tgPending[i].logKey) + "/confirmedAt", String(timeClient.getEpochTime()));
            fbPut(devPath + "/log/" + String(tgPending[i].logKey) + "/confirmedHour", String(getHour()));
            fbPut(devPath + "/log/" + String(tgPending[i].logKey) + "/confirmedMin", String(getMin()));
          }
          ledMode=4;
          externalPending = false;
          tgSendMsg(tgPending[i].chatId,"Confermato! Buona salute",false);
          memset(&tgPending[i],0,sizeof(TgPending));
          lastTgCheck=millis();
          externalDisplayUntil=0;
          buzzerOk();
          delay(1000);
          updateDisplay();
        }
      }
    }
  }
  http.end();
  // Promemoria e allerta
  for (int i=0;i<MAX_PENDING;i++) {
    if (!tgPending[i].valid||tgPending[i].confirmed) continue;
    unsigned long el=nowEpoch-tgPending[i].dispenseTime;
    if (el>=(unsigned long)(TG_REMIND_MIN*60)&&tgPending[i].reminderSent==0) {
      String m="Promemoria: non hai confermato ";
      m+=tgPending[i].medName;
      tgSendMsg(tgPending[i].chatId,m,true);
      waCall("Attenzione+medicina+non+confermata");
      buzzerReminder();
      tgPending[i].reminderSent=nowEpoch;
      addLog("[TG] Promemoria inviato");
    }
    // Timeout doppio (2x alertMin) senza TG_CHAT_ID2: cancella comunque e resetta LED
    if (el>=(unsigned long)(TG_ALERT_MIN*60*2) && strlen(TG_CHAT_ID2)==0) {
      addLog("[TG] Timeout massimo raggiunto - reset LED");
      memset(&tgPending[i],0,sizeof(TgPending));
      bool anyPending = false;
      for (int j=0; j<MAX_PENDING; j++) if (tgPending[j].valid && !tgPending[j].confirmed) { anyPending=true; break; }
      if (!anyPending) { ledMode=1; externalPending=false; updateDisplay(); }
    }
    if (el>=(unsigned long)(TG_ALERT_MIN*60)&&tgPending[i].reminderSent>0&&strlen(TG_CHAT_ID2)>0) {
      String m2="ALLERTA: "; m2+=tgPending[i].medName; m2+=" non confermata!";
      tgSendMsg(TG_CHAT_ID2,m2,false);
      memset(&tgPending[i],0,sizeof(TgPending));
      // Resetta LED e display  nessun pending rimasto
      bool anyPending = false;
      for (int j=0; j<MAX_PENDING; j++) if (tgPending[j].valid && !tgPending[j].confirmed) { anyPending=true; break; }
      if (!anyPending) { ledMode=1; externalPending=false; updateDisplay(); }
      addLog("[TG] Allerta inviata - LED resettato");
    }
  }
}

//  CALLMEBOT 
bool waCall(String msg) {
  if (!WiFi.isConnected()) return false;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setTimeout(30000);
  msg.replace(" ","+");
  String url="https://api.callmebot.com/call.php?phone=";
  url+=WA_PHONE; url+="&text="+msg;
  url+="&apikey="; url+=WA_APIKEY;
  url+="&lang=it-IT";
  http.begin(c, url);
  int code=http.GET();
  http.end();
  addLog("[CALL] Inviato: " + String(code));
  return code==200;
}

//  PARSE JSON 
String parseField(String& json, const char* field) {
  String key="\""; key+=field; key+="\":";
  int idx=json.indexOf(key);
  if (idx==-1) return "";
  idx+=key.length();
  while (idx<(int)json.length()&&json[idx]==' ') idx++;
  String val="";
  if (json[idx]=='"') {
    idx++;
    while (idx<(int)json.length()&&json[idx]!='"') val+=json[idx++];
  } else {
    while (idx<(int)json.length()&&json[idx]!=','&&json[idx]!='}') val+=json[idx++];
  }
  val.trim(); return val;
}
