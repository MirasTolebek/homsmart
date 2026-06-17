#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h> 
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>

// Подключаем наш секретный файл с настройками!
#include "secrets.h"

#define EEPROM_SIZE 16 
#define ADDR_TEMP_ON 0  
#define ADDR_TEMP_OFF 4 

float TEMP_ON;  
float TEMP_OFF; 

#define ONE_WIRE_BUS 4  
#define RELAY_PIN    5  

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

float currentTemp = 0.0; 
bool firstTempReadDone = false; 
unsigned long lastStatusUpdateTime = 0; 

bool pumpStatus = false;       
bool manualMode = false;       
bool notificationsOn = true;   

bool shouldDisconnectWifi = false;
unsigned long wifiDisconnectTimer = 0;

unsigned long lastTimeBotRan = 0;
const unsigned long botRequestInterval = 1500; 

unsigned long lastTimeTempChecked = 0;
const unsigned long tempCheckInterval = 5000;  

// === INLINE КЛАВИАТУРЫ ===
String inlineMainMenu = "[[{\"text\":\"🔄 Обновить статус\", \"callback_data\":\"btn_status\"}],"
                        "[{\"text\":\"🟢 Включить\", \"callback_data\":\"btn_pump_on\"}, {\"text\":\"⚪ Выключить\", \"callback_data\":\"btn_pump_off\"}],"
                        "[{\"text\":\"🤖 Авторежим\", \"callback_data\":\"btn_auto\"}],"
                        "[{\"text\":\"⚙️ Настройки\", \"callback_data\":\"btn_settings\"}]]";

String inlineSettingsMenu = "[[{\"text\":\"🔔 Вкл увед.\", \"callback_data\":\"btn_notif_on\"}, {\"text\":\"🔕 Выкл увед.\", \"callback_data\":\"btn_notif_off\"}],"
                            "[{\"text\":\"⚠️ Выключить Wi-Fi\", \"callback_data\":\"btn_wifi_off\"}],"
                            "[{\"text\":\"◀️ Назад\", \"callback_data\":\"btn_status\"}]]";

String inlineConfirmWifi = "[[{\"text\":\"💀 Подтвердить отключение\", \"callback_data\":\"btn_confirm_wifi\"}],"
                           "[{\"text\":\"❌ Отмена\", \"callback_data\":\"btn_settings\"}]]";

// === ВОЗВРАЩЕННАЯ ВАША ЛОГИКА РЕЛЕ ===
void turnPumpOn() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); 
  pumpStatus = true;
  Serial.println("Реле: ВКЛЮЧЕНО (OUTPUT LOW)");
}

void turnPumpOff() {
  pinMode(RELAY_PIN, INPUT_PULLUP); 
  pumpStatus = false;
  Serial.println("Реле: ВЫКЛЮЧЕНО (INPUT_PULLUP)");
}
// =====================================

String getStatusText() {
  String modeStr = manualMode ? "Ручной 🛠️" : "Автоматический 🤖";
  String pumpStr = pumpStatus ? "ВКЛЮЧЕН 🟢" : "ВЫКЛЮЧЕН ⚪";
  String notifStr = notificationsOn ? "Включены 🔔" : "Выключены 🔕";
  
  String tempStr;
  if (!firstTempReadDone) {
    tempStr = "Опрос датчика... ⏳";
  } else if (currentTemp == DEVICE_DISCONNECTED_C) {
    tempStr = "Ошибка датчика ❌";
  } else {
    tempStr = String(currentTemp, 1) + "°C";
  }
  
  String timeStr;
  if (lastStatusUpdateTime == 0) {
    timeStr = "только что создан";
  } else {
    timeStr = String((millis() - lastStatusUpdateTime) / 1000) + " сек. назад";
  }
  
  String text = "🌡️ *Текущее состояние:* \n\n";
  text += "• Температура печи: *" + tempStr + "*\n";
  text += "• Режим работы: " + modeStr + "\n";
  text += "• Насос: " + pumpStr + "\n";
  text += "• Уведомления: " + notifStr + "\n\n";
  text += "⚙️ *Настройки автоматики:*\n";
  text += "Включение насоса при: " + String(TEMP_ON, 1) + "°C\n";
  text += "Выключение насоса при: " + String(TEMP_OFF, 1) + "°C\n";
  text += "_Обновлено: " + timeStr + "_"; 
  return text;
}

void handleNewMessages(int numNewMessages) {
  Serial.print("📩 Входящих сообщений: ");
  Serial.println(numNewMessages);

  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    
    if (chat_id != CHAT_ID) {
      Serial.println("⛔ Блокировка: Чужой CHAT_ID!");
      bot.sendMessage(chat_id, "Доступ ограничен!", "");
      continue;
    }

    // --- ОБРАБОТКА ТЕКСТОВЫХ СООБЩЕНИЙ ---
    if (bot.messages[i].type == "message") {
      String text = bot.messages[i].text;
      text.trim();
      
      Serial.print("Текст: ");
      Serial.println(text);
      
      if (text.startsWith("/нагрев ")) {
        int spaceIndex = text.indexOf(' '); 
        float newTemp = text.substring(spaceIndex + 1).toFloat(); 
        if (newTemp > 0 && newTemp <= 100) { 
          TEMP_ON = newTemp;
          EEPROM.writeFloat(ADDR_TEMP_ON, TEMP_ON);
          EEPROM.commit(); 
          bot.sendMessage(CHAT_ID, "✅ Температура ВКЛЮЧЕНИЯ сохранена: *" + String(TEMP_ON, 1) + "°C*", "Markdown");
          bot.sendMessageWithInlineKeyboard(CHAT_ID, getStatusText(), "Markdown", inlineMainMenu); 
          lastStatusUpdateTime = millis();
        }
      }
      else if (text.startsWith("/остывание ")) {
        int spaceIndex = text.indexOf(' '); 
        float newTemp = text.substring(spaceIndex + 1).toFloat(); 
        if (newTemp > 0 && newTemp <= 100) {
          TEMP_OFF = newTemp;
          EEPROM.writeFloat(ADDR_TEMP_OFF, TEMP_OFF);
          EEPROM.commit();
          bot.sendMessage(CHAT_ID, "✅ Температура ВЫКЛЮЧЕНИЯ сохранена: *" + String(TEMP_OFF, 1) + "°C*", "Markdown");
          bot.sendMessageWithInlineKeyboard(CHAT_ID, getStatusText(), "Markdown", inlineMainMenu);
          lastStatusUpdateTime = millis();
        }
      }
      else {
        bot.sendMessageWithInlineKeyboard(CHAT_ID, getStatusText(), "Markdown", inlineMainMenu);
        lastStatusUpdateTime = millis();
      }
    }
    
    // --- ОБРАБОТКА НАЖАТИЯ INLINE-КНОПОК ---
    else if (bot.messages[i].type == "callback_query") {
      String callbackData = bot.messages[i].text;
      int message_id = bot.messages[i].message_id; 
      String callbackId = bot.messages[i].query_id; 
      
      Serial.println("--- 🔘 НАЖАТА КНОПКА ---");
      Serial.print("Data: "); Serial.println(callbackData);
      Serial.print("Message ID: "); Serial.println(message_id);
      Serial.print("Query ID: "); Serial.println(callbackId);
      
      bool ans = bot.answerCallbackQuery(callbackId);
      Serial.print("Ответ серверу (answerCallback): "); Serial.println(ans ? "УСПЕХ" : "ОШИБКА");
      
      if (callbackData == "btn_status") {
        bot.sendMessageWithInlineKeyboard(chat_id, getStatusText(), "Markdown", inlineMainMenu, message_id);
        lastStatusUpdateTime = millis();
      }
      else if (callbackData == "btn_pump_on") {
        turnPumpOn();
        manualMode = true;
        bot.sendMessageWithInlineKeyboard(chat_id, getStatusText(), "Markdown", inlineMainMenu, message_id);
        lastStatusUpdateTime = millis();
      }
      else if (callbackData == "btn_pump_off") {
        turnPumpOff();
        manualMode = true;
        bot.sendMessageWithInlineKeyboard(chat_id, getStatusText(), "Markdown", inlineMainMenu, message_id);
        lastStatusUpdateTime = millis();
      }
      else if (callbackData == "btn_auto") {
        manualMode = false;
        bot.sendMessageWithInlineKeyboard(chat_id, getStatusText(), "Markdown", inlineMainMenu, message_id);
        lastStatusUpdateTime = millis();
      }
      else if (callbackData == "btn_settings") {
        bot.sendMessageWithInlineKeyboard(chat_id, "⚙️ *Меню настроек*", "Markdown", inlineSettingsMenu, message_id);
      }
      else if (callbackData == "btn_notif_on") {
        notificationsOn = true;
        bot.sendMessageWithInlineKeyboard(chat_id, getStatusText(), "Markdown", inlineSettingsMenu, message_id);
        lastStatusUpdateTime = millis();
      }
      else if (callbackData == "btn_notif_off") {
        notificationsOn = false;
        bot.sendMessageWithInlineKeyboard(chat_id, getStatusText(), "Markdown", inlineSettingsMenu, message_id);
        lastStatusUpdateTime = millis();
      }
      else if (callbackData == "btn_wifi_off") {
        String warning = "⚠️ *ВНИМАНИЕ!*\nВы собираетесь перевести плату в автономный режим. Связь пропадет!";
        bot.sendMessageWithInlineKeyboard(chat_id, warning, "Markdown", inlineConfirmWifi, message_id);
      }
      else if (callbackData == "btn_confirm_wifi") {
        bot.sendMessage(chat_id, "💀 Wi-Fi отключается. Плата переходит в автономный режим!", "");
        shouldDisconnectWifi = true;
        wifiDisconnectTimer = millis();
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  // === ВОТ ЭТА ВОЛШЕБНАЯ СТРОЧКА ОЖИВИТ КНОПКИ ===
  bot.maxMessageLength = 4096; 
  // ===============================================

  sensors.begin();
  
  turnPumpOff(); 

  EEPROM.begin(EEPROM_SIZE);
  TEMP_ON = EEPROM.readFloat(ADDR_TEMP_ON);
  TEMP_OFF = EEPROM.readFloat(ADDR_TEMP_OFF);

  if (isnan(TEMP_ON) || TEMP_ON < 0 || TEMP_ON > 100) {
    TEMP_ON = 30.0; 
    EEPROM.writeFloat(ADDR_TEMP_ON, TEMP_ON);
  }
  if (isnan(TEMP_OFF) || TEMP_OFF < 0 || TEMP_OFF > 100) {
    TEMP_OFF = 27.0; 
    EEPROM.writeFloat(ADDR_TEMP_OFF, TEMP_OFF);
  }
  EEPROM.commit();

  Serial.print("Подключение к Wi-Fi: ");
  Serial.println(ssid);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi подключен успешно!");
    client.setInsecure(); 
    bot.sendMessageWithInlineKeyboard(CHAT_ID, getStatusText(), "Markdown", inlineMainMenu);
    lastStatusUpdateTime = millis();
  } else {
    Serial.println("\nНе удалось подключиться к Wi-Fi.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Опрос датчика (Раз в 5 секунд)
  if (currentMillis - lastTimeTempChecked >= tempCheckInterval) {
    sensors.requestTemperatures();
    currentTemp = sensors.getTempCByIndex(0); 
    firstTempReadDone = true; 

    if (currentTemp != DEVICE_DISCONNECTED_C) {
      if (!manualMode) {
        if (currentTemp >= TEMP_ON && !pumpStatus) {
          turnPumpOn();
          Serial.println("Автоматика: Насос ВКЛЮЧЕН");
          if (WiFi.status() == WL_CONNECTED && notificationsOn) {
            bot.sendMessage(CHAT_ID, "🔴 *Автоматика:* Печь нагрелась. Насос *ВКЛЮЧЕН*.", "Markdown");
          }
        } 
        else if (currentTemp <= TEMP_OFF && pumpStatus) {
          turnPumpOff();
          Serial.println("Автоматика: Насос ВЫКЛЮЧЕН");
          if (WiFi.status() == WL_CONNECTED && notificationsOn) {
            bot.sendMessage(CHAT_ID, "🔵 *Автоматика:* Печь остыла. Насос *ВЫКЛЮЧЕН*.", "Markdown");
          }
        }
      }
    }
    lastTimeTempChecked = currentMillis;
  }

  // 2. Опрос Telegram 
  if (WiFi.status() == WL_CONNECTED) {
    if (currentMillis - lastTimeBotRan >= botRequestInterval) {
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      
      if (numNewMessages) { 
        handleNewMessages(numNewMessages);
      }
      lastTimeBotRan = currentMillis;
    }
  }

  // 3. Безопасное отключение Wi-Fi
  if (shouldDisconnectWifi && (currentMillis - wifiDisconnectTimer >= 1000)) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    btStop(); 
    shouldDisconnectWifi = false;
    Serial.println("Плата переведена в глубокий автономный режим.");
  }
}