//!!!!! Include necessary libraries:
#include <Arduino.h>
#if defined(ESP32)
#include <WiFi.h>
#endif
#include "time.h"

//--RFID library:
#include "R200.h"
//--Telegram Bot library:
#include <UniversalTelegramBot.h>
//--Arduino JSON library:
#include <ArduinoJson.h>
//--Secure WiFi Client library:
#include <WiFiClientSecure.h>

//--Firebase related:
#include <Firebase_ESP_Client.h>
//--Provide the token generation process info:
#include "addons/TokenHelper.h"
//--Provide the RTDB payload printing info and other helper functions:
#include "addons/RTDBHelper.h"

//!!!!! Define Firebase-related variables:
//--Network credentials:
#define WIFI_SSID "<YOUR WIFI SSID>"
#define WIFI_PASSWORD "<YOUR WIFI PASSWORD>"

//--Firebase project API Key:
#define API_KEY "<FIREBASE_API_KEY>"
//--RTDB URL:
#define DATABASE_URL "<FIREBASE RTDB URL>"

//!!!!! Define Telegram-related variables:
//--Bot token:
#define BOTtoken "<TELEGRAM BOT TOKEN>"
//--TG ID:
#define CHAT_ID "<USER CHAT ID>"

//!!!!! Define other necessary vaiables and objects:
//--Firebase objects:
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
//--Telegram objects:
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
//--Delays for checking new messages:
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;
//--RFID object and variables:
R200 rfid;
String numbers[4] = {"0x"};
String sent[20] = {"0"};
int sent_position = 0;
bool canSend = false;
String path = "";
//--GPS data variables:
String latitude = "";
String longitude = "";
String location = "";
//--GPS timer variables:
unsigned long lastResetTime = 0;
unsigned long gpsTimeout = 60000;
//--Time variables:
String current_date = "";
String current_hour = "";
String current_time = "";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

//!!!!! Functtion for handling location messages:
void handleNewMessages(int numNewMessages) {
  //--Loop through new messages:
  for (int i = 0; i < numNewMessages; i++) {
    //--Access chat ID:
    String chat_id = bot.messages[i].chat_id;
    //--If chat ID is wrong, ignore message:
    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Unauthorized user", "Markdown");
      continue;
    }
    //--Access message text:
    String text = bot.messages[i].text;

    //--If a location is sent...:
    if (bot.messages[i].longitude != 0 || bot.messages[i].latitude != 0) {
      //--Set global coord-s variables:
      latitude = String(bot.messages[i].latitude, 6);
      longitude = String(bot.messages[i].longitude, 6);
      location = latitude + " " + longitude;
      //--Start GPS timer:
      lastResetTime = millis();
      //--Print location:
      Serial.print("Lat: ");
      Serial.println(latitude);
      Serial.print("Long: ");
      Serial.println(longitude);

      //--Form and send message containing coord-s and instructions:
      String message = "Lat: " + latitude + "\n";
      message += "Long: " + longitude + "\n";
      message += "You have 10 minutes to scan products.\nYou will then be required to provide your current location again."; 
      bot.sendMessage(chat_id, message, "Markdown");
    }
    //--If the message is "/start"...:
    else if (text == "/start") {
      //--Send a welcome text:
      String welcome = "Welcome to Ecoton Inventory Management!\nScanner is ready!";
      bot.sendMessage(chat_id, welcome, "Markdown");
    }
    //--If the message is "/check"...:
    else if (text == "/check") {
      //--Send a welcome text:
      String check = "Scanner ready!\n";
      bot.sendMessage(chat_id, check, "Markdown");
    }
    //--If the message is anything else...:
    else {
      //--Send a warning:
      String warning = "Please refrain from sending unnecessary messages!";
      bot.sendMessage(chat_id, warning, "Markdown");
    }
  }
}

bool check_if_sent(String current_number) {
  for (int i = 0; i < 4; i++) {
    if(current_number == sent[i]) {
      return false;
    }
  }
  return true;
}

void reset_sent() {
  for (int i = 0; i < 20; i++) {
    sent[i] = "0";
  }
}

// ########################################################################################################################
void setup() {
  //!!!!! Initialize the Serials in use:
  //--Display Serial:
  Serial.begin(115200);
  Serial.println(__FILE__ __DATE__);
  //--RFID Serial:
  rfid.begin(&Serial2, 115200, 16, 17); // IS IT SERIAL 1 or 2???

  //!!!!! Connect to WiFi:
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  //--Add root certificate for api.telegram.org:
  #ifdef ESP32
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  #endif
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("!!! CONNECTED TO WI-FI !!!");
  Serial.println();

  //!!!!! Set up Firebase connection:
  //--Assign API Key and RTDB URL:
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  //--Sign up:
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("!!! AUTHORIZED IN FIREBASE !!!");
    signupOK = true;
  } else {
    Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }

  //--Assign the callback function for the long running token generation task:
  config.token_status_callback = tokenStatusCallback;

  //--Begin Firebase:
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  //--Configure time:
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// ########################################################################################################################
void loop() {
  //--Reset the SENT UIDs array:
  if (sent_position >= 20) reset_sent();

  //!!!!! Check timer:
  //--If it ran out...:
  if (millis() - lastResetTime >= gpsTimeout) {
    //--Empty coord-s variables:
    latitude = "";
    longitude = "";
    reset_sent();
    Serial.println("!!! GPS Timeout !!!");
    bot.sendMessage(CHAT_ID, "Please send your current location again!", "Markdown");
  }

  //!!!!! Get GPS data from Telegram:
  //--While coord-s variables are empty...:
  while (latitude == "" && longitude == "") {
    //--Check for updates every second:
    if (millis() > lastTimeBotRan + botRequestDelay) {
      //--Check number of new messages:
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

      //--While there are new messages...:
      while (numNewMessages) {
        //--Announce number of new messages:
        Serial.println("Got " + String(numNewMessages) + " messages");
        //--Handle new messages:
        handleNewMessages(numNewMessages);
        //--Update number of new messages:
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
      //--Restart timer for checking updates:
      lastTimeBotRan = millis();
    }
  }

  //!!!!! Establish tm struct:
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  //!!!!! Scan stuff:
  //!!!!! Get RFID data:
  //--Poll RFID:
  rfid.loop();
  rfid.poll();
  //--Print UID to Display Serial:
  rfid.dumpUIDToSerial();
  Serial.println();
  //--Empty the UID array:
  for (int b = 0; b < 4; b++) {
    numbers[b] = "0x";
  }

  //--Loop through the rfid.uid array and form the UID array:
  for (int i = 0; i < 4; i++) {
    Serial.println("ENTERING THE NUMBERS LOOP!");
    for (int j = 0; j < 12; j++) {
      numbers[i] += (rfid.uid[i][j] < 0x10 ? "0" : "");
      //--Add the hexadecimal representation of uid[i]
      numbers[i] += String(rfid.uid[i][j], HEX); 
    }
    Serial.println(numbers[i]);
  }

  //--Send the UIDs from the UID array:
  for(int i = 0; i < 4; i++) {
    Serial.println("ENTERING THE FIREBASE LOOP!");
    if (numbers[i] == "0x000000000000000000000000") {
      continue;
    }
    String current_number = numbers[i];

    canSend = check_if_sent(current_number);

    //--Start the path:
    path = "RFID/" + current_number;
    Serial.print("PATH: ");
    Serial.println(path);

    //!!!!! Get and format time info:
    //--Empty time variables:
    current_date = "";
    current_hour = "";
    current_time = "";
    //--Set the date:
    current_date += String(timeinfo.tm_year + 1900);
    current_date += "-";
    if (timeinfo.tm_mon + 1 < 10) current_date += "0";
    current_date += String(timeinfo.tm_mon + 1);
    current_date += "-";
    if (timeinfo.tm_mday < 10) current_date += "0";
    current_date += String(timeinfo.tm_mday);
    //--Set the hour:
    if (timeinfo.tm_hour + 5 < 10) current_hour += "0";
    current_hour += String(timeinfo.tm_hour + 3);
    //--Set the minutes and seconds:
    if (timeinfo.tm_min < 10) current_hour += "0";
    current_hour += String(timeinfo.tm_min);
    //current_time += ":";
    if (timeinfo.tm_sec < 10) current_time += "0";
    current_time += String(timeinfo.tm_sec);

    //!!!!! Access Firebase, check the time, and store if necessary:
    //--Check if Firebase is connected:
    if (canSend) {
      if (Firebase.ready()) {
        //--If the number is in the database and has not been scanned within the last hour...:
        if (Firebase.RTDB.pathExisted(&fbdo, path)) { //&& !Firebase.RTDB.pathExisted(&fbdo, path + "/" + current_date + "/" + current_hour)) {
          //--Form the complete path:
          path += "/" + current_date + "/" + current_hour + "/" + current_time;
          //--Add current scan into the database:
          if (Firebase.RTDB.setString(&fbdo, path, location + " " + CHAT_ID)) {
            Serial.println("PASSED");
            Serial.println("PATH: " + fbdo.dataPath());
            Serial.println("TYPE: " + fbdo.dataType());
            String bub = "SENT\n";
            bub += current_number;
            bot.sendMessage(CHAT_ID, bub, "Markdown");
            sent[sent_position] = current_number;
            sent_position++;
          } else {
            Serial.println("FAILED");
            Serial.println("REASON: " + fbdo.errorReason());
          }
        }
      }
    }
  }
}
