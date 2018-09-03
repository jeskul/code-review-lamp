#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include "credentials.h"

const auto CONNECTION_RETRIES = 20;
const auto OPEN_REVIEWS_QUERY = "/a/changes/?q=status:open+is:reviewer";
const auto CHANGES_ENDPOINT = "/a/changes/";
const auto REVIEWERS = "/reviewers";
const auto GERRIT_MAGIC_PREFIX = ")]}'\n";

ESP8266WiFiClass wifi;

/**
   Block and indicate an error to the user
*/
void indicateError() {
  pinMode(LED_BUILTIN, OUTPUT);
  while (true) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
  }
}

void setup() {
  Serial.begin(9600);
  // Set WiFi to station mode and disconnect from an AP
  // if it was previously connected.
  wifi.mode(WIFI_STA);
  wifi.disconnect();
  delay(100);

  // Try to connect to the internet.
  wifi.begin(INTERNET_SSID, PASSWORD);
  auto attemptsLeft = CONNECTION_RETRIES;
  Serial.print("Connecting");
  while ((wifi.status() != WL_CONNECTED) && (--attemptsLeft > 0)) {
    delay(500); // Wait a bit before retrying
    Serial.print(".");
  }

  if (attemptsLeft <= 0) {
    Serial.println(" Connection error!");
    indicateError();
  }
  Serial.println(" Connection success");

  HTTPClient http;
  http.begin(String(GERRIT_URL + OPEN_REVIEWS_QUERY));
  http.setAuthorization("nikosp", "RRXWEQA9aguvXlsES2XA0ayMbiJ2JtpHety0r3LeJw");
  auto httpCode = http.GET();

  if (httpCode < 0 || httpCode != HTTP_CODE_OK) {
    Serial.printf("GET failed, code: %s\n", http.errorToString(httpCode).c_str());
    indicateError();
  }
  auto payload = http.getString();
  http.end();

  // Gerrit inserts a "magic prefix" into the JSON response for security reasons
  payload.replace(GERRIT_MAGIC_PREFIX, "");

  DynamicJsonDocument doc;
  deserializeJson(doc, payload);
  auto reviews = doc.as<JsonArray>();

  for (auto& review : reviews) {
    
    String reviewNumber = review["_number"];
    Serial.println(reviewNumber);
    // Get all reviewers and their ratings
    //http://dell-laptop:8080/a/changes/101/reviewers
    Serial.println(String(GERRIT_URL + CHANGES_ENDPOINT + reviewNumber + REVIEWERS));
    http.begin(String(GERRIT_URL + CHANGES_ENDPOINT + reviewNumber + REVIEWERS));
    httpCode = http.GET();

    if (httpCode < 0 || httpCode != HTTP_CODE_OK) {
      Serial.printf("GET failed, code: %s\n", http.errorToString(httpCode).c_str());
      indicateError();
    }
    

    payload = http.getString();
    http.end();

    payload.replace(GERRIT_MAGIC_PREFIX, "");
    
    Serial.println(payload);
    deserializeJson(doc, payload);

    auto approvals = doc.as<JsonArray>();
    auto reviewsDone = 0;
    for (auto& approval : approvals) {
        auto codeReview = approval["approvals"]["Code-Review"];
        if (codeReview != "0") {
            reviewsDone++;
        }
    }
    Serial.printf("Reviews done: %d\n", reviewsDone);
    if (reviewsDone > 3) {
        // remove yourself from review
    } else {
        
    }
  }

}

void loop() {

}
