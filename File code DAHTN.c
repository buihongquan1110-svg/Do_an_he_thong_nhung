#define BLYNK_TEMPLATE_ID "TMPL6w5ixp-SO"
#define BLYNK_TEMPLATE_NAME "SmartParking"
#define BLYNK_AUTH_TOKEN "CtWnkKU8Yjh4diMfX5W7U_RuMohcqjyu"


#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>     
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <BlynkSimpleEsp32.h>
#include <FirebaseESP32.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <string.h> 

// ================= 1. CẤU HÌNH PIN =================
#define SS_PIN        5
#define RST_PIN       15
#define SERVO_IN_PIN  4   
#define SERVO_OUT_PIN 14 
#define LED_PIN       2

// ================= 2. THÔNG SỐ HỆ THỐNG =================
#define FIREBASE_HOST "smartparking-esp32dt300-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "Km0cK5n0zCa8s3n6GtDLWEvtJXytzkxo3dXl8Ng4"

#define MAX_MEMBERS 10  
#define MAX_SLOTS 10    
#define ENTRY_FEE 5000  
#define WDT_TIMEOUT 8 

MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo servoIn, servoOut;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences pref;

struct Member { 
    char rfid[13]; 
    long balance; 
    bool isInside; 
};

struct ParkingSlot { char rfid[13]; unsigned long entryTime; };
struct LogData { char rfid[13]; char action[15]; int fee; long balance; };

Member memberList[MAX_MEMBERS]; 
int memberCount = 0;
bool adminMode = false;
int availableSlots = MAX_SLOTS;
long totalRevenue = 0; 
ParkingSlot slots[MAX_SLOTS];

const char masterCard[] = "BAAFF280"; 

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);

QueueHandle_t rfidQueue;
QueueHandle_t logQueue;
SemaphoreHandle_t lcdMutex, dataMutex, blynkMutex;

// Các biến chống dội thẻ
char lastRfidRead[13] = "";
unsigned long lastReadTime = 0;

// Biến giao tiếp giữa Blynk và ProcessTask (Tránh Deadlock mạng)
char blynkRemoveRfid[13] = "";
volatile bool blynkRemoveFlag = false;

// Prototypes
void syncWithFirebase();
void saveAllToFlash();
void saveMemberToFlash(int index);
void updateSystemState(const char* title);

// ================= 3. QUẢN LÝ BỘ NHỚ & ĐỒNG BỘ =================

void saveGlobalStatsToFlash() {
    if (!pref.begin("parking", false)) return;
    pref.putInt("count", memberCount);
    pref.putLong("revenue", totalRevenue);
    pref.end();
}

void saveMemberToFlash(int i) {
    if (!pref.begin("parking", false)) return;
    char key[10];
    snprintf(key, sizeof(key), "r%d", i); pref.putString(key, memberList[i].rfid);
    snprintf(key, sizeof(key), "b%d", i); pref.putLong(key, memberList[i].balance);
    snprintf(key, sizeof(key), "s%d", i); pref.putBool(key, memberList[i].isInside);
    pref.end();
}

void saveAllToFlash() {
    if (!pref.begin("parking", false)) return;
    pref.clear(); 
    pref.putInt("count", memberCount);
    pref.putLong("revenue", totalRevenue);
    for (int i = 0; i < memberCount; i++) {
        char key[10];
        snprintf(key, sizeof(key), "r%d", i); pref.putString(key, memberList[i].rfid);
        snprintf(key, sizeof(key), "b%d", i); pref.putLong(key, memberList[i].balance);
        snprintf(key, sizeof(key), "s%d", i); pref.putBool(key, memberList[i].isInside);
    }
    pref.end();
}

void loadFromFlash() {
    pref.begin("parking", true);
    memberCount = pref.getInt("count", 0);
    if(memberCount > MAX_MEMBERS) memberCount = MAX_MEMBERS;
    
    totalRevenue = pref.getLong("revenue", 0);
    for (int i = 0; i < memberCount; i++) {
        char key[10];
        snprintf(key, sizeof(key), "r%d", i); 
        String rfidVal = pref.getString(key, "");
        memset(memberList[i].rfid, 0, 13);
        strncpy(memberList[i].rfid, rfidVal.c_str(), 12);
        
        snprintf(key, sizeof(key), "b%d", i); memberList[i].balance = pref.getLong(key, 0);
        snprintf(key, sizeof(key), "s%d", i); memberList[i].isInside = pref.getBool(key, false);
    }
    pref.end();
}

void syncWithFirebase() {
    // Ép hệ thống đợi Firebase kết nối tối đa 5 giây trước khi kéo dữ liệu
    int waitFb = 0;
    while (!Firebase.ready() && waitFb < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waitFb++;
    }
    
    if (!Firebase.ready()) return; // Nếu mạng quá yếu đành bỏ qua để không treo mạch

    if (Firebase.get(firebaseData, "/AuthorizedCards")) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        memberCount = 0; 
        if (firebaseData.dataType() == "json") {
            FirebaseJson &json = firebaseData.jsonObject();
            size_t len = json.iteratorBegin();
            String key, value; int type, count = 0;
            for (size_t i = 0; i < len && count < MAX_MEMBERS; i++) {
                json.iteratorGet(i, type, key, value);
                if (type == FirebaseJson::JSON_OBJECT) {
                    memset(memberList[count].rfid, 0, 13);
                    strncpy(memberList[count].rfid, key.c_str(), 12);
                    FirebaseJsonData jsonData;
                    json.get(jsonData, key + "/balance");
                    memberList[count].balance = jsonData.intValue;
                    json.get(jsonData, key + "/isInside");
                    memberList[count].isInside = jsonData.boolValue;
                    count++;
                }
            }
            json.iteratorEnd(); memberCount = count;
        }
        xSemaphoreGive(dataMutex);
    }
    if (Firebase.getInt(firebaseData, "/Revenue/Total")) totalRevenue = firebaseData.intData();
    saveAllToFlash(); 
}

// ================= 4. UTILS & DISPLAY =================

void updateSystemState(const char* title) {
    int currentSlots;
    long currentRev;
    
    // Đọc biến dùng chung an toàn (Race Condition Fix)
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50))) {
        currentSlots = availableSlots;
        currentRev = totalRevenue;
        xSemaphoreGive(dataMutex);
    } else { return; }

    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100))) {
        char buf[17];
        snprintf(buf, sizeof(buf), "%-16s", title);
        lcd.setCursor(0, 0); lcd.print(buf);
        snprintf(buf, sizeof(buf), "S:%-2d R:%-7ld", currentSlots, currentRev);
        lcd.setCursor(0, 1); lcd.print(buf);
        xSemaphoreGive(lcdMutex);
    }

    if (xSemaphoreTake(blynkMutex, pdMS_TO_TICKS(50))) {
        Blynk.virtualWrite(V1, currentSlots); 
        Blynk.virtualWrite(V3, currentRev);
        xSemaphoreGive(blynkMutex);
    }
}

void openGate(bool isIn) {
    digitalWrite(LED_PIN, HIGH);
    Servo &s = isIn ? servoIn : servoOut;
    s.attach(isIn ? SERVO_IN_PIN : SERVO_OUT_PIN); 
    s.write(90); 
    vTaskDelay(pdMS_TO_TICKS(2500)); 
    s.write(0); 
    vTaskDelay(pdMS_TO_TICKS(500));
    s.detach(); 
    digitalWrite(LED_PIN, LOW);
}

// ================= 5. BLYNK INTERACTION =================

BLYNK_WRITE(V10) { 
    const char* reqStr = param.asStr();
    if (strlen(reqStr) < 4) return;
    
    // Báo cờ cho ProcessTask xử lý để tránh sập mạng
    strncpy(blynkRemoveRfid, reqStr, 12);
    blynkRemoveRfid[12] = '\0';
    strupr(blynkRemoveRfid); 
    
    blynkRemoveFlag = true; 
}

// ================= 6. FREERTOS TASKS =================

void RFIDTask(void *pv) {
    char rfidArr[13];
    while (1) {
        if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
            memset(rfidArr, 0, sizeof(rfidArr));
            for (byte i = 0; i < mfrc522.uid.size && i < 6; i++) {
                sprintf(&rfidArr[i * 2], "%02X", mfrc522.uid.uidByte[i]);
            }
            
            // Chống dội thẻ (Debounce)
            if (strcmp(rfidArr, lastRfidRead) != 0 || (millis() - lastReadTime) > 2000) {
                strcpy(lastRfidRead, rfidArr);
                lastReadTime = millis();
                xQueueSend(rfidQueue, rfidArr, pdMS_TO_TICKS(10));
            }
            
            mfrc522.PICC_HaltA(); 
            mfrc522.PCD_StopCrypto1(); 
        }
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

void ProcessTask(void *pv) {
    char rfidArr[13]; LogData log;
    while (1) {
        // Xử lý cờ xóa thẻ từ Blynk
        if (blynkRemoveFlag) {
            blynkRemoveFlag = false;
            char reqRfid[13];
            strcpy(reqRfid, blynkRemoveRfid);

            xSemaphoreTake(dataMutex, portMAX_DELAY);
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (strcasecmp(reqRfid, slots[i].rfid) == 0) {
                    memset(slots[i].rfid, 0, 13); slots[i].entryTime = 0;
                    if (availableSlots < MAX_SLOTS) availableSlots++;
                    break;
                }
            }
            int idx = -1;
            for (int i = 0; i < memberCount; i++) {
                if (strcasecmp(reqRfid, memberList[i].rfid) == 0) { idx = i; break; }
            }
            if (idx != -1) {
                for (int i = idx; i < memberCount - 1; i++) memberList[i] = memberList[i + 1];
                memberCount--;
            }
            xSemaphoreGive(dataMutex);
            
            saveAllToFlash(); 
            
            strncpy(log.rfid, reqRfid, 12); strcpy(log.action, "REM");
            xQueueSend(logQueue, &log, pdMS_TO_TICKS(100));
            
            updateSystemState("Blynk: Nuked!");
        }

        // Xử lý quét thẻ
        if (xQueueReceive(rfidQueue, rfidArr, pdMS_TO_TICKS(100))) {
            strupr(rfidArr); 
            
            if (strcasecmp(rfidArr, masterCard) == 0) {
                adminMode = !adminMode;
                updateSystemState(adminMode ? "MODE: ADMIN" : "MODE: USER");
                vTaskDelay(pdMS_TO_TICKS(1500)); 
                continue;
            }

            if (adminMode) {
                xSemaphoreTake(dataMutex, portMAX_DELAY);
                int idx = -1;
                for (int i = 0; i < memberCount; i++) {
                    if (strcasecmp(rfidArr, memberList[i].rfid) == 0) { idx = i; break; }
                }
                
                if (idx != -1) { 
                    if (memberList[idx].isInside) {
                        for (int i = 0; i < MAX_SLOTS; i++) {
                            if (strcasecmp(rfidArr, slots[i].rfid) == 0) {
                                memset(slots[i].rfid, 0, 13); slots[i].entryTime = 0;
                                if (availableSlots < MAX_SLOTS) availableSlots++;
                                break;
                            }
                        }
                    }
                    for (int i = idx; i < memberCount - 1; i++) memberList[i] = memberList[i + 1];
                    memberCount--;
                    xSemaphoreGive(dataMutex);
                    
                    updateSystemState("Admin: Removed"); 
                    saveAllToFlash(); 
                    
                    strncpy(log.rfid, rfidArr, 12); strcpy(log.action, "REM");
                    xQueueSend(logQueue, &log, pdMS_TO_TICKS(100));
                    
                } else if (memberCount < MAX_MEMBERS) { 
                    strncpy(memberList[memberCount].rfid, rfidArr, 12);
                    memberList[memberCount].balance = 100000;
                    memberList[memberCount].isInside = false;
                    memberCount++;
                    xSemaphoreGive(dataMutex);
                    
                    updateSystemState("Admin: Added");
                    saveAllToFlash();
                    
                    strncpy(log.rfid, rfidArr, 12); strcpy(log.action, "ADD");
                    log.balance = 100000;
                    xQueueSend(logQueue, &log, pdMS_TO_TICKS(100));
                } else {
                    xSemaphoreGive(dataMutex);
                    updateSystemState("List Full!");
                }
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }

            if (!timeClient.isTimeSet()) { updateSystemState("Time Error"); continue; }

            xSemaphoreTake(dataMutex, portMAX_DELAY);
            int mIdx = -1;
            for (int i = 0; i < memberCount; i++) {
                if (strcasecmp(rfidArr, memberList[i].rfid) == 0) { mIdx = i; break; }
            }
            
            if (mIdx == -1) { 
                xSemaphoreGive(dataMutex); updateSystemState("Invalid Card"); continue; 
            }

            if (memberList[mIdx].isInside) { 
                int pIdx = -1;
                for (int i = 0; i < MAX_SLOTS; i++) {
                    if (strcasecmp(rfidArr, slots[i].rfid) == 0) { pIdx = i; break; }
                }

                if (pIdx != -1) {
                    unsigned long now = timeClient.getEpochTime();
                    unsigned long dur = (now > slots[pIdx].entryTime) ? (now - slots[pIdx].entryTime) : 0;
                    int fee = ((dur / 3600) + 1) * ENTRY_FEE;
                    
                    if (memberList[mIdx].balance >= fee) {
                        memberList[mIdx].balance -= fee;
                        memberList[mIdx].isInside = false;
                        totalRevenue += fee;
                        if (availableSlots < MAX_SLOTS) availableSlots++;
                        memset(slots[pIdx].rfid, 0, 13); 
                        
                        xSemaphoreGive(dataMutex); 
                        
                        char msgBuf[17];
                        snprintf(msgBuf, sizeof(msgBuf), "Paid: %d", fee);
                        updateSystemState(msgBuf);
                        openGate(false);
                        
                        strncpy(log.rfid, rfidArr, 12); strcpy(log.action, "EXIT");
                        log.fee = fee; log.balance = memberList[mIdx].balance;
                        xQueueSend(logQueue, &log, pdMS_TO_TICKS(100)); 
                        
                        saveMemberToFlash(mIdx);
                        saveGlobalStatsToFlash();
                    } else {
                        xSemaphoreGive(dataMutex);
                        updateSystemState("Low Balance");
                    }
                } else {
                    xSemaphoreGive(dataMutex);
                    updateSystemState("Slot Error");
                }
            } else { 
                if (availableSlots > 0) {
                    int empty = -1;
                    for(int i=0; i<MAX_SLOTS; i++) if(strlen(slots[i].rfid) == 0) { empty = i; break; }
                    
                    if (empty != -1) {
                        availableSlots--;
                        memberList[mIdx].isInside = true;
                        strncpy(slots[empty].rfid, rfidArr, 12);
                        slots[empty].entryTime = timeClient.getEpochTime();
                        
                        xSemaphoreGive(dataMutex);

                        updateSystemState("Welcome In");
                        openGate(true);
                        
                        strncpy(log.rfid, rfidArr, 12); strcpy(log.action, "ENTER");
                        log.fee = 0; log.balance = memberList[mIdx].balance;
                        xQueueSend(logQueue, &log, pdMS_TO_TICKS(100)); 
                        
                        saveMemberToFlash(mIdx);
                        saveGlobalStatsToFlash();
                    } else xSemaphoreGive(dataMutex);
                } else {
                    xSemaphoreGive(dataMutex);
                    updateSystemState("Parking Full");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void FirebaseTask(void *pv) {
    LogData log;
    while (1) {
        if (xQueueReceive(logQueue, &log, portMAX_DELAY)) {
            if (Firebase.ready()) {
                if (strcmp(log.action, "ENTER") == 0 || strcmp(log.action, "EXIT") == 0) {
                    FirebaseJson json;
                    json.add("rfid", log.rfid); json.add("action", log.action);
                    json.add("fee", log.fee); json.add("balance", (int)log.balance);
                    json.add("time", timeClient.getFormattedTime());
                    
                    Firebase.pushJSON(firebaseData, "/ParkingLogs", json);
                    Firebase.setInt(firebaseData, String("/AuthorizedCards/") + log.rfid + "/balance", log.balance);
                    
                    bool state = (strcmp(log.action, "ENTER") == 0);
                    Firebase.setBool(firebaseData, String("/AuthorizedCards/") + log.rfid + "/isInside", state);
                    Firebase.setInt(firebaseData, "/Revenue/Total", totalRevenue);
                } 
                else if (strcmp(log.action, "ADD") == 0) {
                    Firebase.setInt(firebaseData, String("/AuthorizedCards/") + log.rfid + "/balance", log.balance);
                    Firebase.setBool(firebaseData, String("/AuthorizedCards/") + log.rfid + "/isInside", false);
                } 
                else if (strcmp(log.action, "REM") == 0) {
                    Firebase.deleteNode(firebaseData, String("/AuthorizedCards/") + log.rfid);
                }
            }
        }
    }
}

void NetworkTask(void *pv) {
    esp_task_wdt_add(NULL); 
    while (1) {
        if (WiFi.status() == WL_CONNECTED) {
            if (xSemaphoreTake(blynkMutex, pdMS_TO_TICKS(10))) { 
                Blynk.run();
                xSemaphoreGive(blynkMutex);
            }
            timeClient.update();
        }
        esp_task_wdt_reset(); 
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ================= 7. SETUP =================

void setup() {
    Serial.begin(115200);
    lcd.init(); lcd.backlight();
    lcd.print("Smart Parking");


    WiFiManager wm;
    wm.setConfigPortalTimeout(120); 
    
    lcd.setCursor(0, 1);
    lcd.print("Connecting WiFi.");
    bool res = wm.autoConnect("SmartParking_AP", "12345678"); 
    
    if (!res) {
        lcd.clear(); lcd.print("WiFi Timeout!");
        delay(3000); ESP.restart(); 
    }
    
    lcd.clear(); lcd.print("WiFi Connected!");
    delay(1000);
    
    // Fix kẹt đồng bộ giờ (Thử tối đa 10 lần, nếu mất Internet thì bỏ qua để mạch vẫn chạy được)
    timeClient.begin();
    lcd.setCursor(0, 1);
    lcd.print("Syncing Time...");
    Serial.print("Đang lấy giờ chuẩn từ NTP");

    int ntpRetry = 0;
    while (!timeClient.update() && ntpRetry < 10) {
        timeClient.forceUpdate();
        Serial.print(".");
        delay(500); 
        ntpRetry++;
    }
    
    if (ntpRetry >= 10) {
        Serial.println("\nLỗi mạng: Bỏ qua NTP, sẽ đồng bộ ngầm sau!");
        lcd.clear();
        lcd.print("No Internet!");
        delay(2000);
    } else {
        Serial.println("\nĐồng bộ giờ thành công!");
        lcd.clear();
        lcd.print("Time Updated!");
        delay(1000);
    }

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&twdt_config); 

    for(int i=0; i<MAX_SLOTS; i++) memset(slots[i].rfid, 0, 13);
    
    loadFromFlash(); 
    
    lcdMutex = xSemaphoreCreateMutex();
    dataMutex = xSemaphoreCreateMutex();
    blynkMutex = xSemaphoreCreateMutex(); 
    
    rfidQueue = xQueueCreate(10, 13);
    logQueue = xQueueCreate(10, sizeof(LogData));

    config.host = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    Blynk.config(BLYNK_AUTH_TOKEN); 
    
    SPI.begin(); mfrc522.PCD_Init(); 
    pinMode(LED_PIN, OUTPUT);
    
    // Đồng bộ an toàn
    syncWithFirebase(); 

    xTaskCreatePinnedToCore(RFIDTask, "RFID", 3072, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(ProcessTask, "PROC", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(FirebaseTask, "FB", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(NetworkTask, "NET", 4096, NULL, 1, NULL, 0);

    updateSystemState("System Ready");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}