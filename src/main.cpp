#include <Arduino.h>
#include <Bounce2.h>
#include <Preferences.h>
#include <BleGamepad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
#include "image.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "webpages.h"

#define TFT_CS 10
#define TFT_RST 1
#define TFT_DC 11
#define TFT_MOSI 13
#define TFT_SCLK 12

#define BOUNCE_WITH_PROMPT_DETECTION
#define numOfButtons 14

// webui

struct Config
{
  String ssid;
  String wifipassword;
  String httpuser;
  String httppassword;
  int webserverporthttp;
};

Config config;
bool shouldReboot = false;
AsyncWebServer *server;

// tft
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= tft.height())
    return 0;
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

// init
int debouncetime = 0;

bool configmode = true;

BleGamepad bleGamepad;

Bounce debouncers[numOfButtons];
byte buttonPins[numOfButtons] = {21, 47, 48, 45, 38, 39, 40, 41, 42, 2, 20, 19, 18, 17};
byte physicalButtons[numOfButtons] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
char *buttonNames[numOfButtons] = {"Left", "Down", "Right", "Up", "Start", "Select", "A", "B", "X", "Y", "L1", "R1", "L2", "R2"};
int bindings[numOfButtons];

Preferences preferences;

int socdmode;

// map buttons to physical buttons and save to preferences

void loadPreferences()
{
  bindings[0] = preferences.getInt("Left", 0);
  bindings[1] = preferences.getInt("Down", 1);
  bindings[2] = preferences.getInt("Right", 2);
  bindings[3] = preferences.getInt("Up", 3);
  bindings[4] = preferences.getInt("Start", 4);
  bindings[5] = preferences.getInt("Select", 5);
  bindings[6] = preferences.getInt("A", 6);
  bindings[7] = preferences.getInt("B", 7);
  bindings[8] = preferences.getInt("X", 8);
  bindings[9] = preferences.getInt("Y", 9);
  bindings[10] = preferences.getInt("L1", 10);
  bindings[11] = preferences.getInt("R1", 11);
  bindings[12] = preferences.getInt("L2", 12);
  bindings[13] = preferences.getInt("R2", 13);
  socdmode = preferences.getInt("socdmode", 1);
}

void initGamepad()
{
  BleGamepadConfiguration bleGamepadConfig;

  bleGamepadConfig.setControllerType(CONTROLLER_TYPE_GAMEPAD);

  bleGamepadConfig.setButtonCount(numOfButtons);

  bleGamepadConfig.setHatSwitchCount(1);

  bleGamepadConfig.setIncludeStart(true);

  bleGamepadConfig.setIncludeSelect(true);

  bleGamepadConfig.setAutoReport(false);

  bleGamepad.begin(&bleGamepadConfig);
}

String humanReadableSize(const size_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " KB";
  else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
  else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
}

String listFiles(bool ishtml = false);

String listFiles(bool ishtml) {
  String returnText = "";
  Serial.println("Listing files stored on SPIFFS");
  File root = SPIFFS.open("/");
  File foundfile = root.openNextFile();
  if (ishtml) {
    returnText += "<table><tr><th align='left'>Name</th><th align='left'>Size</th><th></th><th></th></tr>";
  }
  while (foundfile) {
    if (ishtml) {
      returnText += "<tr align='left'><td>" + String(foundfile.name()) + "</td><td>" + humanReadableSize(foundfile.size()) + "</td>";
      returnText += "<td><button onclick=\"downloadDeleteButton(\'" + String(foundfile.name()) + "\', \'download\')\">Download</button>";
      returnText += "<td><button onclick=\"downloadDeleteButton(\'" + String(foundfile.name()) + "\', \'delete\')\">Delete</button></tr>";
    } else {
      returnText += "File: " + String(foundfile.name()) + " Size: " + humanReadableSize(foundfile.size()) + "\n";
    }
    foundfile = root.openNextFile();
  }
  if (ishtml) {
    returnText += "</table>";
  }
  root.close();
  foundfile.close();
  return returnText;
}

void notFound(AsyncWebServerRequest *request)
{
  String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
  Serial.println(logmessage);
  request->send(404, "text/plain", "Not found");
}

// used by server.on functions to discern whether a user has the correct httpapitoken OR is authenticated by username and password
bool checkUserWebAuth(AsyncWebServerRequest *request)
{
  bool isAuthenticated = false;

  if (request->authenticate(config.httpuser.c_str(), config.httppassword.c_str()))
  {
    Serial.println("is authenticated via username and password");
    isAuthenticated = true;
  }
  return isAuthenticated;
}

String processor(const String &var)
{
  if (var == "FIRMWARE")
  {
    return String("0.01");
  }

  if (var == "FREESPIFFS")
  {
    return humanReadableSize((SPIFFS.totalBytes() - SPIFFS.usedBytes()));
  }

  if (var == "USEDSPIFFS")
  {
    return humanReadableSize(SPIFFS.usedBytes());
  }

  if (var == "TOTALSPIFFS")
  {
    return humanReadableSize(SPIFFS.totalBytes());
  }

  // Add a return statement at the end of the function
  return String();
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  // make sure authenticated before allowing upload
  if (checkUserWebAuth(request))
  {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
    Serial.println(logmessage);

    if (!index)
    {
      logmessage = "Upload Start: " + String(filename);
      // open the file on first call and store the file handle in the request object
      request->_tempFile = SPIFFS.open("/" + filename, "w");
      Serial.println(logmessage);
    }

    if (len)
    {
      // stream the incoming chunk to the opened file
      request->_tempFile.write(data, len);
      logmessage = "Writing file: " + String(filename) + " index=" + String(index) + " len=" + String(len);
      Serial.println(logmessage);
    }

    if (final)
    {
      logmessage = "Upload Complete: " + String(filename) + ",size: " + String(index + len);
      // close the file handle as the upload is now done
      request->_tempFile.close();
      Serial.println(logmessage);
      request->redirect("/");
    }
  }
  else
  {
    Serial.println("Auth: Failed");
    return request->requestAuthentication();
  }
}

void configureWebServer()
{
  // configure web server

  // if url isn't found
  server->onNotFound(notFound);

  // run handleUpload function when any file is uploaded
  server->onFileUpload(handleUpload);

  // visiting this page will cause you to be logged out
  server->on("/logout", HTTP_GET, [](AsyncWebServerRequest *request)
             {
    request->requestAuthentication();
    request->send(401); });

  // presents a "you are now logged out webpage
  server->on("/logged-out", HTTP_GET, [](AsyncWebServerRequest *request)
             {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
    Serial.println(logmessage);
    request->send_P(401, "text/html", logout_html, processor); });

  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request)
             {
               String logmessage = "Client:" + request->client()->remoteIP().toString() + +" " + request->url();

               if (checkUserWebAuth(request))
               {
                 logmessage += " Auth: Success";
                 Serial.println(logmessage);
                 request->send_P(200, "text/html", index_html, processor);
               }
               else
               {
                 logmessage += " Auth: Failed";
                 Serial.println(logmessage);
                 return request->requestAuthentication();
               } });

  server->on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request)
             {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();

    if (checkUserWebAuth(request)) {
      request->send(200, "text/html", reboot_html);
      logmessage += " Auth: Success";
      Serial.println(logmessage);
      shouldReboot = true;
    } else {
      logmessage += " Auth: Failed";
      Serial.println(logmessage);
      return request->requestAuthentication();
    } });

  server->on("/listfiles", HTTP_GET, [](AsyncWebServerRequest *request)
             {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
    if (checkUserWebAuth(request)) {
      logmessage += " Auth: Success";
      Serial.println(logmessage);
      request->send(200, "text/plain", listFiles(true));
    } else {
      logmessage += " Auth: Failed";
      Serial.println(logmessage);
      return request->requestAuthentication();
    } });

  server->on("/file", HTTP_GET, [](AsyncWebServerRequest *request)
             {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
    if (checkUserWebAuth(request)) {
      logmessage += " Auth: Success";
      Serial.println(logmessage);

      if (request->hasParam("name") && request->hasParam("action")) {
        const char *fileName = request->getParam("name")->value().c_str();
        const char *fileAction = request->getParam("action")->value().c_str();

        logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url() + "?name=" + String(fileName) + "&action=" + String(fileAction);

        if (!SPIFFS.exists(fileName)) {
          Serial.println(logmessage + " ERROR: file does not exist");
          request->send(400, "text/plain", "ERROR: file does not exist");
        } else {
          Serial.println(logmessage + " file exists");
          if (strcmp(fileAction, "download") == 0) {
            logmessage += " downloaded";
            request->send(SPIFFS, fileName, "application/octet-stream");
          } else if (strcmp(fileAction, "delete") == 0) {
            logmessage += " deleted";
            SPIFFS.remove(fileName);
            request->send(200, "text/plain", "Deleted File: " + String(fileName));
          } else {
            logmessage += " ERROR: invalid action param supplied";
            request->send(400, "text/plain", "ERROR: invalid action param supplied");
          }
          Serial.println(logmessage);
        }
      } else {
        request->send(400, "text/plain", "ERROR: name and action params required");
      }
    } else {
      logmessage += " Auth: Failed";
      Serial.println(logmessage);
      return request->requestAuthentication();
    } });
}

// handles uploads to the filserver

void setup()
{
  // load settings

  if (!SPIFFS.begin(true))
  {
    ESP.restart();
  }

  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);

  tft.init(170, 320); // Init ST7789 172x320

  tft.setRotation(1);

  uint16_t time = millis();
  time = millis() - time;

  preferences.begin("settings", false);

  loadPreferences();

  debouncetime = preferences.getInt("debouncetime", 10);

  for (byte currentPinIndex = 0; currentPinIndex < numOfButtons; currentPinIndex++)
  {
    pinMode(buttonPins[currentPinIndex], INPUT_PULLUP);

    debouncers[currentPinIndex] = Bounce();
    debouncers[currentPinIndex].attach(buttonPins[currentPinIndex]); // After setting up the button, setup the Bounce instance :
    debouncers[currentPinIndex].interval(5);
  }

  // bind keys

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);

  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(0, 0);
  tft.println("Starting up...");
  tft.println();
  tft.println("Hold any key for key config");
  tft.println("Press Start to initiate WebUi");

  time = millis() - time;

  bool settings = false;
  bool calibrate = false;

  while (millis() < time + 2000)

  {
    for (byte currentIndex = 0; currentIndex < numOfButtons; currentIndex++)
    {
      debouncers[currentIndex].update();

      if (debouncers[currentIndex].fell())
      {
        if (bindings[4] == currentIndex)
        {
          settings = true;
          break;
        }

        else
          calibrate = true;
      }

      if (debouncers[currentIndex].rose())
      {
        calibrate = false;
      }
    }
  }

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(3);

  if (calibrate)
  {
    for (int i = 0; i < numOfButtons; i++)
    {
      bool button_pressed = false;

      tft.setTextColor(ST77XX_GREEN);
      tft.setCursor(0, 0);
      tft.print("Button " + String(buttonNames[i]));

      while (!button_pressed)
      {
        for (byte currentIndex = 0; currentIndex < numOfButtons; currentIndex++)
        {
          debouncers[currentIndex].update();

          if (debouncers[currentIndex].fell())
          {
            button_pressed = true;
            bindings[i] = currentIndex;
            preferences.putInt(buttonNames[i], currentIndex);
            delay(100);
          }
        }
      }

      tft.setTextColor(ST77XX_BLACK);
      tft.setCursor(0, 0);
      tft.print("Button " + String(buttonNames[i]));
    }

    // DPAD_CENTERED, DPAD_UP, DPAD_UP_RIGHT, DPAD_RIGHT, DPAD_DOWN_RIGHT, DPAD_DOWN
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_GREEN);
    tft.setCursor(0, 0);
    tft.print("Config Saved");
    delay(1000);
    tft.fillScreen(ST77XX_BLACK);
  }

  loadPreferences();

  if (settings)
  {
    config.ssid = "skibidibox";
    config.wifipassword = "skibidibox";
    config.httpuser = "skibidibox";
    config.httppassword = "skibidibox";
    config.webserverporthttp = 80;

    WiFi.begin(config.ssid.c_str(), config.wifipassword.c_str());
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
    }

    server = new AsyncWebServer(config.webserverporthttp);
    configureWebServer();

    server->begin();

    tft.println("WebUi started");
    tft.println("Connect to " + config.ssid);
    tft.println("IP: " + WiFi.localIP().toString());

    while (true)
    {
      if (shouldReboot)
      {
        ESP.restart();
      }
    }
  }

    initGamepad();

    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(tft_output);

    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, image, sizeof(image));
    TJpgDec.drawJpg(0, 0, image, sizeof(image));
}


void loop()
{
  if (bleGamepad.isConnected())
  {

    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;

    for (byte currentIndex = 0; currentIndex < numOfButtons; currentIndex++)
    {
      debouncers[currentIndex].update();

      if (debouncers[currentIndex].read() == LOW)
      {
        if (bindings[0] == currentIndex)
        {
          left = true;
        }
        if (bindings[1] == currentIndex)
        {
          down = true;
        }
        if (bindings[2] == currentIndex)
        {
          right = true;
        }
        if (bindings[3] == currentIndex)
        {
          up = true;
        }
      }

      if (debouncers[currentIndex].fell())
      {
        if (bindings[4] == currentIndex)
        {
          bleGamepad.pressStart();
        }

        if (bindings[5] == currentIndex)
        {
          bleGamepad.pressSelect();
        }

        else
          for (byte currentButtonIndex = 6; currentButtonIndex < numOfButtons; currentButtonIndex++)
          {
            if (bindings[currentButtonIndex] == currentIndex)
            {
              bleGamepad.press(currentButtonIndex - 5);
            }
          }
      }
      else if (debouncers[currentIndex].rose())
      {
        if (bindings[4] == currentIndex)
        {
          bleGamepad.releaseStart();
        }

        if (bindings[5] == currentIndex)
        {
          bleGamepad.releaseSelect();
        }

        else
          for (byte currentButtonIndex = 6; currentButtonIndex < numOfButtons; currentButtonIndex++)
          {
            if (bindings[currentButtonIndex] == currentIndex)
            {
              bleGamepad.release(currentButtonIndex - 5);
            }
          }
      }
    }

    bleGamepad.setHat1(DPAD_CENTERED);

    if (up and down)
    {
      down = false;
    }

    if (left and right)
    {
      left = false;
      right = false;
    }

    if (left)
    {
      bleGamepad.setHat1(DPAD_LEFT);
    }

    if (right)
    {
      bleGamepad.setHat1(DPAD_RIGHT);
    }

    if (up)
    {
      bleGamepad.setHat1(DPAD_UP);
    }

    if (down)
    {
      bleGamepad.setHat1(DPAD_DOWN);
    }

    if (left and up)
    {
      bleGamepad.setHat1(DPAD_UP_LEFT);
    }

    if (left and down)
    {
      bleGamepad.setHat1(DPAD_DOWN_LEFT);
    }

    if (right and up)
    {
      bleGamepad.setHat1(DPAD_UP_RIGHT);
    }

    if (right and down)
    {
      bleGamepad.setHat1(DPAD_DOWN_RIGHT);
    }

    bleGamepad.sendReport();
  }
}
