#include <Arduino.h>
#include <string.h>
#include "TFT_eSPI.h"  // EPaper class provided by Seeed_GFX
#include <Preferences.h>

#ifndef EPAPER_ENABLE
  #error "EPAPER_ENABLE is not defined. Check src/driver.h and the Seeed_GFX configuration."
#endif

// EE05 4.2-inch monochrome panel.
static constexpr uint16_t IMAGE_WIDTH = 400;
static constexpr uint16_t IMAGE_HEIGHT = 300;
static constexpr uint16_t ROW_BYTES = (IMAGE_WIDTH + 7) / 8;
static constexpr size_t BITMAP_BYTES = static_cast<size_t>(ROW_BYTES) * IMAGE_HEIGHT;
static constexpr size_t FRAME_HEADER_BYTES = 16;

// EE05 user buttons: external pull-ups; pressed = LOW.
static constexpr uint8_t KEY1_PIN = 2;  // D1 / GPIO2
static constexpr uint8_t KEY2_PIN = 3;  // D2 / GPIO3
static constexpr uint8_t KEY3_PIN = 8;  // D9 / GPIO8

static const uint8_t keyPins[] = {KEY1_PIN, KEY2_PIN, KEY3_PIN};

EPaper epaper;

// One 1-bit, 400x300 image occupies 15,000 bytes.
// This is kept in RAM so new images can be received without reflashing.
static uint8_t imageBuffer[BITMAP_BYTES];
static bool imageReceived = false;
// This flag is stored in ESP32 NVS. The ePaper panel is bistable, so its
// physical image remains visible through a controller reset or power loss.
// Remembering that an image is already on the panel prevents setup() from
// overwriting it with the welcome screen after a USB CDC reset.
static bool panelHasImage = false;
static bool preferencesReady = false;
static Preferences displayPreferences;
static uint32_t lastReadyMessageMs = 0;

static uint16_t readLE16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readLE32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0UL - (crc & 1UL);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

// Read exactly `length` bytes. The timeout is an inactivity timeout, not a
// total-transfer timeout, so slower USB serial delivery is tolerated.
static bool readExact(uint8_t *destination, size_t length, uint32_t timeoutMs) {
  size_t received = 0;
  uint32_t lastByteMs = millis();

  while (received < length) {
    while (Serial.available() > 0 && received < length) {
      const int value = Serial.read();
      if (value >= 0) {
        destination[received++] = static_cast<uint8_t>(value);
        lastByteMs = millis();
      }
    }

    if (received == length) {
      return true;
    }

    if (millis() - lastByteMs >= timeoutMs) {
      return false;
    }

    delay(1);
  }

  return true;
}

static void drawReadyScreen() {
  const int w = epaper.width();
  const int h = epaper.height();

  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawRect(0, 0, w, h, TFT_BLACK);
  epaper.setTextSize(2);
  epaper.drawString("EE05 IMAGE RECEIVER", 20, 90);
  epaper.drawString("400 x 300 MONOCHROME", 20, 130);
  epaper.drawString("Choose an image in the PC sender", 20, 180);
  epaper.drawString("and click SEND TO EE05", 20, 210);
  epaper.update();
}

static void drawReceivedImage(bool inverted) {
  const uint16_t foreground = inverted ? TFT_WHITE : TFT_BLACK;
  const uint16_t background = inverted ? TFT_BLACK : TFT_WHITE;

  epaper.fillScreen(background);
  epaper.drawBitmap(
    0, 0,
    imageBuffer,
    IMAGE_WIDTH,
    IMAGE_HEIGHT,
    foreground,
    background
  );
  epaper.update();

}

static void clearDisplay() {
  epaper.fillScreen(TFT_WHITE);
  epaper.update();
  imageReceived = false;
  panelHasImage = false;
  if (preferencesReady) {
    displayPreferences.putBool("hasImage", false);
  }
}

static void sendReadyMessage() {
  Serial.println("EE05_READY");
  lastReadyMessageMs = millis();
}

// Host frame format, all integers little-endian:
// 4 bytes "EIMG", uint16 width, uint16 height, uint32 payload length,
// uint32 CRC-32, followed by 128-byte stop-and-wait data blocks. The board
// replies ACK:<cumulative-byte-count> after each successfully read block.
static void receiveImageFrame() {
  uint8_t header[FRAME_HEADER_BYTES];

  if (!readExact(header, sizeof(header), 5000)) {
    Serial.println("ERR_HEADER_TIMEOUT");
    return;
  }

  if (memcmp(header, "EIMG", 4) != 0) {
    Serial.println("ERR_BAD_MAGIC");
    return;
  }

  const uint16_t width = readLE16(header + 4);
  const uint16_t height = readLE16(header + 6);
  const uint32_t payloadLength = readLE32(header + 8);
  const uint32_t expectedCrc = readLE32(header + 12);

  if (width != IMAGE_WIDTH || height != IMAGE_HEIGHT ||
      payloadLength != BITMAP_BYTES) {
    Serial.println("ERR_IMAGE_FORMAT");
    return;
  }

  Serial.println("RECEIVING_IMAGE");
  Serial.flush();

  // Stop-and-wait transfer: the PC sends one small block, then waits for
  // this acknowledgement before sending the next block. This prevents the
  // ESP32-S3 USB CDC receive buffer from overflowing on a fast PC sender.
  static constexpr size_t CHUNK_BYTES = 128;
  size_t offset = 0;
  while (offset < BITMAP_BYTES) {
    const size_t chunkLength = (BITMAP_BYTES - offset < CHUNK_BYTES)
                                   ? (BITMAP_BYTES - offset)
                                   : CHUNK_BYTES;

    if (!readExact(imageBuffer + offset, chunkLength, 10000)) {
      Serial.printf("ERR_PAYLOAD_TIMEOUT offset=%u\n", static_cast<unsigned>(offset));
      Serial.flush();
      return;
    }

    offset += chunkLength;
    Serial.printf("ACK:%u\n", static_cast<unsigned>(offset));
    Serial.flush();
  }

  const uint32_t actualCrc = crc32(imageBuffer, BITMAP_BYTES);
  if (actualCrc != expectedCrc) {
    Serial.println("ERR_CRC");
    return;
  }

  imageReceived = true;
  drawReceivedImage(false);
  panelHasImage = true;
  if (preferencesReady) {
    displayPreferences.putBool("hasImage", true);
  }
  Serial.println("DISPLAYED");
}

static void handleButton(uint8_t index) {
  if (index == 0) {
    if (imageReceived) {
      drawReceivedImage(false);
      Serial.println("Key1: normal image");
    } else if (!panelHasImage) {
      drawReadyScreen();
    } else {
      Serial.println("The previous image is still on the panel; resend it from the PC to reload it into RAM.");
    }
  } else if (index == 1) {
    if (imageReceived) {
      drawReceivedImage(true);
      Serial.println("Key2: inverted image");
    } else if (panelHasImage) {
      Serial.println("The previous image is retained on the panel; send it again to enable inversion.");
    }
  } else {
    clearDisplay();
    Serial.println("Key3: white screen");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(1000);

  const uint32_t serialStart = millis();
  while (!Serial && (millis() - serialStart < 2000)) {
    delay(10);
  }

  Serial.println();
  Serial.println("=== EE05 4.2-inch USB image receiver ===");
  Serial.printf("PSRAM: %u bytes\n", static_cast<unsigned>(ESP.getPsramSize()));

  for (uint8_t i = 0; i < 3; ++i) {
    pinMode(keyPins[i], INPUT_PULLUP);
  }

  Serial.println("Initializing ePaper...");
  epaper.begin();
  Serial.printf("Panel size: %d x %d\n", epaper.width(), epaper.height());

  if (epaper.width() != IMAGE_WIDTH || epaper.height() != IMAGE_HEIGHT) {
    Serial.println("WARNING: panel dimensions do not match 400x300.");
  }

  // Read a small persistent marker. We do not store the 15 KB bitmap in NVS:
  // the ePaper panel itself retains the last displayed image without power.
  preferencesReady = displayPreferences.begin("ee05display", false);
  if (preferencesReady) {
    panelHasImage = displayPreferences.getBool("hasImage", false);
  } else {
    Serial.println("WARNING: NVS preferences unavailable; startup screen will be shown.");
  }

  if (panelHasImage) {
    // Do not refresh the display here. Leave the existing ePaper image alone.
    Serial.println("Previous image detected; keeping the existing ePaper contents.");
  } else {
    drawReadyScreen();
  }

  Serial.println("Buttons: Key1=normal, Key2=invert, Key3=clear");
  sendReadyMessage();
}

void loop() {
  // The ready heartbeat helps the desktop sender find the board even if USB
  // serial opening resets the ESP32-S3.
  if (millis() - lastReadyMessageMs >= 2000) {
    sendReadyMessage();
  }

  if (Serial.available() > 0) {
    receiveImageFrame();
    sendReadyMessage();
  }

  for (uint8_t i = 0; i < 3; ++i) {
    if (digitalRead(keyPins[i]) != LOW) {
      continue;
    }

    delay(30);
    if (digitalRead(keyPins[i]) != LOW) {
      continue;
    }

    handleButton(i);
    while (digitalRead(keyPins[i]) == LOW) {
      delay(10);
    }
    delay(30);
  }
}
