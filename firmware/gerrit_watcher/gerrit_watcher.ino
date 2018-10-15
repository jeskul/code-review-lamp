#include <vector>
#include <stdint.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

// Keep your project specific credentials in a non-version controlled
// `credentials.h` file and set the `NO_CREDENTIALS_HEADER` to false.
#define NO_CREDENTIALS_HEADER false
#if NO_CREDENTIALS_HEADER == true
const auto INTERNET_SSID = "your_ssid";
const auto PASSWORD = "your_password";
const String GERRIT_URL = "http://your_gerrit_url:8080";
const auto GERRIT_USERNAME = "your_gerrit_username";
const auto GERRIT_HTTP_PASSWORD = "your_gerrit_http_password";
#else
#include "credentials.h"
#endif

enum class Effect {PULSE, RADAR, COOL_RADAR};
const auto MY_EFFECT = Effect::RADAR;

struct RGBColor {
  RGBColor(int r = 0, int g = 0, int b = 0) : red{r}, green{g}, blue{b} {}
  int red;
  int green;
  int blue;
};

struct HSVColor {
  /**
    @param h    Hue ranged          [0,360)
    @param s    Saturation ranged   [0,100]
    @param v    Value ranged        [0,100]
  */
  HSVColor(int h = 0, int s = 0, int v = 0) : hue{h}, saturation{s}, value{v} {}
  int hue;
  int saturation;
  int value;

  /**
    Converts HSV colors to RGB that can be used for Neopixels
    so that we can adjust the brightness of the colors.
    Code adapted from: https://stackoverflow.com/a/14733008

    @param hsv  The color in HSV format to convert
    @return     The equivalent color in RGB
  */
  RGBColor toRGB() const {
    // Scale the HSV values to the expected range
    auto rangedHue = map(hue, 0, 359, 0, 255);
    auto rangedSat = map(saturation, 0, 100, 0, 255);
    auto rangedVal = map(value, 0, 100, 0, 255);

    if (rangedSat == 0) {
      return {rangedVal, rangedVal, rangedVal};
    }

    auto region = rangedHue / 43;
    auto remainder = (rangedHue - (region * 43)) * 6;

    auto p = (rangedVal * (255 - rangedSat)) >> 8;
    auto q = (rangedVal * (255 - ((rangedSat * remainder) >> 8))) >> 8;
    auto t = (rangedVal * (255 - ((rangedSat * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
      case 0:
        return {rangedVal, t, p};
      case 1:
        return {q, rangedVal, p};
      case 2:
        return {p, rangedVal, t};
      case 3:
        return {p, q, rangedVal};
      case 4:
        return {t, p, rangedVal};
      default:
        return {rangedVal, p, q};
    }
  }
};

const auto NEOPIXEL_PIN = 15;
const auto NEOPIXEL_RING_SIZE = 16;
const auto DIM_WINDOW = 10000UL;
const auto CHECK_FOR_REVIEWS_INTERVAL = 20000UL;
const auto ERROR_BLINK_INTERVAL = 250UL;
const auto WAIT_FOR_GERRIT_RESPONSE = 50UL;
const auto RECONNECT_TIMEOUT = 100UL;
const auto RETRY_CONNECTION_INTERVAL = 500UL;
const auto CONNECTION_RETRIES = 20;
const auto OPEN_REVIEWS_QUERY = "/a/changes/?q=status:open+is:reviewer";
const String CHANGES_ENDPOINT = GERRIT_URL + "/a/changes/";
const auto REVIEWERS = "/reviewers/";
const auto DELETE = "/delete";
const auto ALL_REVIEWS_ASSIGNED_URL = GERRIT_URL + OPEN_REVIEWS_QUERY;
const auto GERRIT_REVIEW_NUMBER_ATTRIBUTE = "_number";
const auto GERRIT_REVIEW_APPROVAL_ATTRIBUTE = "Code-Review";
const auto GERRIT_REVIEW_OWNERID_ATTRIBUTE = "_account_id";
const auto ENOUGH_CONDUCTED_REVIEWS = 2;

const HSVColor KINDA_ORANGE (10, 100, 100);
const HSVColor MELLOW_YELLOW (30, 100, 100);
const HSVColor ALIEN_GREEN (100, 100, 100);
const HSVColor ALMOST_WHITE (293, 4, 70);
const HSVColor GREEK_BLUE (227, 100, 100);
const HSVColor GOTH_PURPLE (315, 100, 100);
const HSVColor BLOOD_RED (0, 100, 100);

Adafruit_NeoPixel ring(NEOPIXEL_RING_SIZE, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

/**
   Maps Gerrit user IDs to lamp colors, adapt accordingly.
   @param userId  Gerrit user ID
   @return The HSV color to match the specific user
*/
HSVColor toColor(const String& userId) {
  switch (userId.toInt()) {
    case 1000037: // nm
      return MELLOW_YELLOW;
    case 1000079: // dj
      return ALIEN_GREEN;
    case 1000078: // jk
      return GOTH_PURPLE;
    case 1000039: // nj
      return KINDA_ORANGE;
    case 1000036: // dp
      return BLOOD_RED;
    case 1000354: // fb
      return GREEK_BLUE;
    default: // Developer from another team
      return ALMOST_WHITE;
  }
}

/**
   Display an error pattern on the neopixels
   @param neopixels The neopixels to display the error
*/
void indicateError(Adafruit_NeoPixel& neopixels) {
  // Blink red LEDs sequentially to indicate an error
  for (auto pixel = 0; pixel < neopixels.numPixels(); pixel++) {
    neopixels.setPixelColor(pixel, 200, 0, 0);
    neopixels.show();
    delay(ERROR_BLINK_INTERVAL);
    neopixels.setPixelColor(pixel, 0, 0, 0);
    neopixels.show();
    delay(ERROR_BLINK_INTERVAL);
  }
}

/**
   @param url    The URL to execute a GET request
   @param key    The key to look inside the incoming JSON stream
   @return       A list with all the values of the specific key
*/
std::vector<String> getStreamAttribute(const String& url, const String& key) {
  HTTPClient http;
  http.begin(url);
  http.setAuthorization(GERRIT_USERNAME, GERRIT_HTTP_PASSWORD);
  auto httpCode = http.GET();

  if (httpCode < 0 || httpCode != HTTP_CODE_OK) {
    Serial.printf("[%s] GET failed with code '%s' for key '%s'\r\n", __FUNCTION__, http.errorToString(httpCode).c_str(), key.c_str());
    http.end();
    return {};
  }
  delay(WAIT_FOR_GERRIT_RESPONSE);

  auto documentLength = http.getSize();
  auto stream = http.getStream();

  std::vector<String> keyValues;
  if (http.connected() && (documentLength > 0 || documentLength == -1)) {
    while (stream.available()) {
      // Parse the value of the key when found
      String line = stream.readStringUntil('\n');
      line.trim();
      line.replace("\"", "");
      // Clear out unnecessary characters
      if (line.startsWith(key)) {
        line.replace(key, "");
        line.replace(",", "");
        line.replace(":", "");
        line.trim();
        // By now it should include just the information we are interested in
        keyValues.push_back(line);
      }
    }
  }

  if (keyValues.empty()) {
    Serial.printf("Warning - Key not found: %s\n\r", key.c_str());
  }
  http.end();

  return keyValues;
}

/**
   (Re)connects the module to WiFi
*/
void connectToWifi() {
  // Set WiFi to station mode & disconnect from an AP if previously connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(RECONNECT_TIMEOUT);

  // Try to connect to the internet
  WiFi.begin(INTERNET_SSID, PASSWORD);
  auto attemptsLeft = CONNECTION_RETRIES;
  Serial.print("Connecting");
  while ((WiFi.status() != WL_CONNECTED) && (--attemptsLeft > 0)) {
    delay(RETRY_CONNECTION_INTERVAL); // Wait a bit before retrying
    Serial.print(".");
  }

  if (attemptsLeft <= 0) {
    Serial.println(" Connection error!");
    indicateError(ring);
  } else {
    Serial.println(" Connection success");
  }
}

/**
    Get all open reviews that our gerrit user has been assigned to,
    then figure out whether enough developers have code-reviewed
    each review. If this has not happened, return a color
    (mapped to a developer) for every un-reviewed review.
    Once a review has been reviewed adequately, remove ourselves
    from it.

    @return HSV colors for every unfinished code review
*/
std::vector<HSVColor> getColorsForUnfinishedReviews() {
  std::vector<HSVColor> colorsToShow;
  Serial.println("Getting all reviews assigned to us");
  auto reviews = getStreamAttribute(ALL_REVIEWS_ASSIGNED_URL, GERRIT_REVIEW_NUMBER_ATTRIBUTE);
  for (auto& review : reviews) {
    // Get all approvals for the specific review
    Serial.printf("Getting all approvals for review %s\n\r", review.c_str());
    auto getChangeUrl = CHANGES_ENDPOINT + review;
    auto getReviewersUrl = CHANGES_ENDPOINT + review + REVIEWERS;
    auto approvals = getStreamAttribute(getReviewersUrl, GERRIT_REVIEW_APPROVAL_ATTRIBUTE);
    // Measure how many reviews have been conducted (i.e. approval is NOT `0`)
    auto conductedReviews = 0;
    for (auto& approval : approvals) {
      if (approval != "0") {
        conductedReviews++;
      }
    }

    if (conductedReviews < ENOUGH_CONDUCTED_REVIEWS) {
      auto ownerId = getStreamAttribute(getChangeUrl, GERRIT_REVIEW_OWNERID_ATTRIBUTE);
      if (!ownerId.empty()) {
        colorsToShow.push_back(toColor(ownerId.front()));
      }
    } else {
      Serial.printf("We got enough reviews in %s, no need to dim\n\r", review.c_str());
    }
  }

  return colorsToShow;
}

/**
   Set the specified RGB color to all the pixels

   @param neopixels The neopixel structure to set color
   @param rgbColor  The RGB color to set the pixels
*/

void setAllPixelColor(Adafruit_NeoPixel& neopixels, RGBColor& rgbColor) {
  switch(MY_EFFECT) {
    case Effect::RADAR:
      setRadarEffect(neopixels, rgbColor);
      break;
    case Effect::PULSE:
      setPulseEffect(neopixels, rgbColor);
      break;
    case Effect::COOL_RADAR:
      setCoolRadarEffect(neopixels, rgbColor);
      break;
    default:
      setRadarEffect(neopixels, rgbColor);
      break;
  }
}

/**
   Perform some radar effect
   @param neopixels The neopixel structure to set color
   @param rgbColor  The RGB color to set the pixels
*/
void setRadarEffect(Adafruit_NeoPixel& neopixels, RGBColor& rgbColor) {
  const auto pixels = neopixels.numPixels();
  static const auto SLICES = 3;

  static unsigned int startingPixel = 0;

  // Does not matter if it rotates back to 0
  startingPixel++;

  // Slice the lamp in parts where the first and brightest one is our radar effect
  // while the rest have a progressively dimmer color.
  for (auto slice = 0; slice < SLICES; slice++) {
    for (auto pixel = slice * pixels / SLICES + startingPixel; pixel < (slice + 1) * pixels / SLICES + startingPixel; pixel++) {
      neopixels.setPixelColor(pixel % pixels, rgbColor.red, rgbColor.green, rgbColor.blue);
    }

    rgbColor.red   /= 2;
    rgbColor.green /= 2;
    rgbColor.blue  /= 2;
  }
}

/**
   Perform some cool radar effect
   @param neopixels The neopixel structure to set color
   @param rgbColor  The RGB color to set the pixels
*/
void setCoolRadarEffect(Adafruit_NeoPixel& neopixels, RGBColor& rgbColor) {
  const auto pixels = neopixels.numPixels();

  static unsigned int startingPixel = 0;
  
  // Does not matter if it rotates back to 0
  startingPixel++;
  
  for (auto pixel = 0 + startingPixel; pixel < 3*pixels/5 + startingPixel; pixel++) {
    neopixels.setPixelColor(pixel%pixels, rgbColor.red, rgbColor.green, rgbColor.blue);
  }

  RGBColor rgb1;
  rgb1.red   = rgbColor.green;
  rgb1.green = rgbColor.blue;
  rgb1.blue  = rgbColor.red;

  for (auto pixel = 3*pixels/5 + startingPixel; pixel < 4*pixels/5 + startingPixel; pixel++) {
    neopixels.setPixelColor(pixel%pixels, rgb1.red, rgb1.green, rgb1.blue);
  }

  RGBColor rgb2;
  rgb2.red   = rgbColor.blue;
  rgb2.green = rgbColor.red;
  rgb2.blue  = rgbColor.green;
  
  for (auto pixel = 4*pixels/5 + startingPixel; pixel < pixels + startingPixel; pixel++) {
    neopixels.setPixelColor(pixel%pixels, rgb2.red, rgb2.green, rgb2.blue);
  }
}

/**
  Perform some pulse effect
   @param neopixels The neopixel structure to set color
   @param rgbColor  The RGB color to set the pixels
*/
void setPulseEffect(Adafruit_NeoPixel& neopixels, RGBColor& rgbColor) {
  for (auto pixel = 0; pixel < neopixels.numPixels(); pixel++) {
    neopixels.setPixelColor(pixel, rgbColor.red, rgbColor.green, rgbColor.blue);
  }
}

/**
   Dim for all the supplied colors throughout the specified window
   @param neopixels The neopixel structure to dim
   @param hsvColors The HSV colors to be dimmed
*/
void dimWithColors(Adafruit_NeoPixel& neopixels, std::vector<HSVColor>& hsvColors) {
  if (hsvColors.empty()) {
    Serial.println("All code is reviewed, good job");
    return;
  }
  // Dim every color within the designated time window
  // The effect we are after is the more unfinished reviews
  // the faster the neopixels will dim
  auto timeSlotForEachColor = DIM_WINDOW / hsvColors.size();
  for (const auto& hsvColor : hsvColors) {
    auto rgb = hsvColor.toRGB();
    // The time slot has to be evenly divided among the intervals necessary
    // to dim it all the way up and down
    auto dimInterval = (timeSlotForEachColor / hsvColor.value) / 2;
    // Dim up every pixel for the current color
    for (auto intensity = 0; intensity < hsvColor.value; intensity++) {
      // Get the RGB value of the currently dimmed HSV color
      auto rgbColor = HSVColor(hsvColor.hue, hsvColor.saturation, intensity).toRGB();
      setAllPixelColor(neopixels, rgbColor);
      neopixels.show();
      delay(dimInterval);
    }

    // Dim down every pixel for the current color
    for (auto intensity = hsvColor.value; intensity >= 0; intensity--) {
      // Get the RGB value of the currently dimmed HSV color
      auto rgbColor = HSVColor(hsvColor.hue, hsvColor.saturation, intensity).toRGB();
      setAllPixelColor(neopixels, rgbColor);
      neopixels.show();
      delay(dimInterval);
    }
  }
}

void setup() {
  Serial.begin(9600);
  ring.begin();
  ring.show(); // Initialize all pixels to 'off'
  connectToWifi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWifi();
  } else {
    auto hsvColors = getColorsForUnfinishedReviews();
    dimWithColors(ring, hsvColors);
  }
  delay(CHECK_FOR_REVIEWS_INTERVAL);
}
