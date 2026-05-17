#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#define CODEBREAKER
#define CODEMAKER
#include <ECE4180MasterMind.h>
#include <DealerClassifier_inferencing.h>
#define VECTOR_SIZE 4
#define ARRAY_SIZE 1296
#define BASE 6

// ==============================
// BLE / Game constants
// ==============================
#define DEVICE_NAME  "Server"
#define SERVICE_UUID "2006"
#define CHAR_UUID    "0001"

#define LED_PIN     18
#define NUM_LEDS    1
#define AI_BUTTON   7

// ==============================
// BLE globals
// ==============================
static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pCharacteristic = nullptr;

// ==============================
// RTOS globals
// ==============================
SemaphoreHandle_t aiBtnSemaphore;
SemaphoreHandle_t predictionMutex;
QueueHandle_t codeQueue;
QueueHandle_t feedbackQueue;

// ==============================
// Game / ML globals
// ==============================
CodeMaker dealer;
Adafruit_NeoPixel pixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

uint8_t possibilities[ARRAY_SIZE][VECTOR_SIZE];
bool active[ARRAY_SIZE];

float classifierConfidence[3] = {0, 0, 0};
uint8_t currentPrediction = 0;

// ==============================
// ISR
// ==============================
volatile unsigned long lastAIPress = 0;

void IRAM_ATTR onAIPredictButton() {
  unsigned long now = millis();
  if (now - lastAIPress < 300) return;  // debounce
  lastAIPress = now;
  
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(aiBtnSemaphore, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ==============================
// ML helper
// ==============================
void classifyDealerStyle(uint8_t* code) {
  int uniqueCount = 0;
  int isDescending = 1;
  int maxV = code[0];

  for (int i = 0; i < 4; i++) {
    bool isUnique = true;
    for (int j = 0; j < i; j++) {
      if (code[i] == code[j]) {
        isUnique = false;
        break;
      }
    }
    if (isUnique) uniqueCount++;
  }

  for (int i = 1; i < 4; i++) {
    if (code[i] > maxV) maxV = code[i];
  }

  for (int i = 0; i < 3; i++) {
    if (!(code[i] > code[i + 1])) {
      isDescending = 0;
      break;
    }
  }

  float features[7] = {
    (float)code[0], (float)code[1], (float)code[2], (float)code[3],
    (float)uniqueCount, (float)isDescending, (float)maxV
  };

  signal_t signal;
  numpy::signal_from_buffer(features, 7, &signal);

  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
  if (res != EI_IMPULSE_OK) {
    Serial.println("Classifier failed");
    return;
  }

  xSemaphoreTake(predictionMutex, portMAX_DELAY);
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    classifierConfidence[ix] += result.classification[ix].value;
  }

  int best = 0;
  for (int i = 1; i < 3; i++) {
    if (classifierConfidence[i] > classifierConfidence[best]) {
      best = i;
    }
  }
  currentPrediction = best;
  xSemaphoreGive(predictionMutex);

  Serial.printf("ML updated from code [%d %d %d %d]\n", code[0], code[1], code[2], code[3]);
}

// ==============================
// BLE callbacks
// ==============================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    Serial.printf("Client connected: %s\n", connInfo.getAddress().toString().c_str());
    pServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("Client disconnected, reason=%d. Restarting advertising.\n", reason);
    NimBLEDevice::startAdvertising();
  }
} serverCallbacks;

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string val = pCharacteristic->getValue();
    if (val.length() == 2) {
      uint8_t fb[2];
      fb[0] = (uint8_t)val[0];
      fb[1] = (uint8_t)val[1];
      xQueueSend(feedbackQueue, fb, 0);
      Serial.printf("Received feedback -> black=%d white=%d\n", fb[0], fb[1]);
    }
    else if (val.length() == 4) {
      uint8_t code[4];
      for (int i = 0; i < 4; i++) code[i] = (uint8_t)val[i];
      xQueueSend(codeQueue, code, 0);
      Serial.printf("Received code sample -> [%d %d %d %d]\n", code[0], code[1], code[2], code[3]);
    }
  }
} chrCallbacks;

// ==============================
// Game logic
// ==============================
void prePrune(uint8_t predictedStyle) {
  for (int i = 0; i < ARRAY_SIZE; i++) {
    if (!active[i]) continue;
    bool keep = false;

    switch (predictedStyle) {
      case 0: {
        keep = true;
        for (int j = 0; j < VECTOR_SIZE; j++) {
          if (possibilities[i][j] > 2) {
            keep = false;
            break;
          }
        }
        break;
      }
      case 1: {
        keep = (possibilities[i][0] > possibilities[i][1] &&
                possibilities[i][1] > possibilities[i][2] &&
                possibilities[i][2] > possibilities[i][3]);
        break;
      }
      case 2: {
        int counts[BASE] = {0};
        for (int j = 0; j < VECTOR_SIZE; j++) {
          counts[possibilities[i][j]]++;
        }
        int pairCount = 0;
        keep = true;
        for (int j = 0; j < BASE; j++) {
          if (counts[j] == 2) pairCount++;
          else if (counts[j] != 0) { keep = false; break; }
        }
        if (pairCount != 2) keep = false;
        break;
      }
      default:
        keep = true;
        break;
    }

    if (!keep) active[i] = false;
  }
}

int getBestGuess() {
  int bestGuessIdx = -1;
  int minMaxRemaining = 2000;

  Serial.println("Calculating best guess...");
  uint32_t start = millis();

  for (int i = 0; i < ARRAY_SIZE; i++) {
    if (i == 0) {
      int activeCount = 0;
      for (int k = 0; k < ARRAY_SIZE; k++) if (active[k]) activeCount++;
      Serial.printf("Active codes remaining: %d\n", activeCount);
    }
    if (i % 200 == 0) Serial.print(".");

    int scoreCount[5][5] = {0};
    for (int j = 0; j < ARRAY_SIZE; j++) {
      if (!active[j]) continue;
      uint8_t check[2];
      dealer.compare(check, possibilities[i], possibilities[j]);
      scoreCount[check[0]][check[1]]++;
    }

    int maxForThisGuess = 0;
    for (int b = 0; b <= 4; b++) {
      for (int w = 0; w <= 4; w++) {
        if (scoreCount[b][w] > maxForThisGuess)
          maxForThisGuess = scoreCount[b][w];
      }
    }

    if (maxForThisGuess < minMaxRemaining) {
      minMaxRemaining = maxForThisGuess;
      bestGuessIdx = i;
    } else if (maxForThisGuess == minMaxRemaining && active[i] && !active[bestGuessIdx]) {
      bestGuessIdx = i;
    }
  }

  Serial.printf("\nDone! Took %d ms. Best Index: %d\n", (millis() - start), bestGuessIdx);
  return bestGuessIdx;
}

void prune(uint8_t* lastGuess, uint8_t* results) {
  for (int i = 0; i < ARRAY_SIZE; i++) {
    if (!active[i]) continue;
    uint8_t reference[2];
    dealer.compare(reference, possibilities[i], lastGuess);
    if (reference[0] != results[0] || reference[1] != results[1]) {
      active[i] = false;
    }
  }
}

void populateArray(uint8_t (*array)[VECTOR_SIZE], uint32_t size) {
  uint8_t counters[VECTOR_SIZE] = {0};
  for (uint32_t i = 0; i < size; i++) {
    active[i] = true;
    memcpy(array[i], counters, VECTOR_SIZE);
    counters[0]++;
    for (uint8_t k = 0; k < VECTOR_SIZE - 1; k++) {
      if (counters[k] >= BASE) {
        counters[k] = 0;
        counters[k + 1]++;
      }
    }
  }
}

// ==============================
// RTOS tasks
// ==============================
void mlTask(void* pvParameters) {
  uint8_t code[4];
  while (1) {
    if (xQueueReceive(codeQueue, code, portMAX_DELAY) == pdTRUE) {
      classifyDealerStyle(code);
    }
  }
}

void aiTask(void* pvParameters) {
  populateArray(possibilities, ARRAY_SIZE);
  bool hasPruned = false;

  while (1) {
    if (xSemaphoreTake(aiBtnSemaphore, portMAX_DELAY) == pdTRUE) {

      if (!hasPruned) {
        xSemaphoreTake(predictionMutex, portMAX_DELAY);
        uint8_t pred = currentPrediction;
        xSemaphoreGive(predictionMutex);

        // Fix model label ordering (model outputs are swapped for 0 and 1)
        uint8_t correctedPred = (pred == 0 ? 1 : pred == 1 ? 0 : 2);

        Serial.print("Predicted style: ");
        Serial.println(correctedPred == 0 ? "LOW_NUMBERS" :
                   correctedPred == 1 ? "DESCENDING" : "TWO_PAIRS");
        prePrune(correctedPred);
        hasPruned = true;
      }

      int bestIdx = getBestGuess();

      uint8_t guess[4];
      memcpy(guess, possibilities[bestIdx], 4);
      pCharacteristic->setValue(guess, 4);
      pCharacteristic->notify();
      Serial.printf("AI sent guess -> [%d %d %d %d]\n",
                    guess[0], guess[1], guess[2], guess[3]);

      uint8_t fb[2];
      if (xQueueReceive(feedbackQueue, fb, portMAX_DELAY) == pdTRUE) {
        Serial.printf("Feedback: black=%d white=%d\n", fb[0], fb[1]);

        if (fb[0] == VECTOR_SIZE) {
          Serial.println("Solved!");
          populateArray(possibilities, ARRAY_SIZE);
          hasPruned = false;
        } else {
          prune(possibilities[bestIdx], fb);
        }
      }
    }
  }
}

// ==============================
// Setup / loop
// ==============================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting S3 BLE Server + AI/ML");

  aiBtnSemaphore = xSemaphoreCreateBinary();
  predictionMutex = xSemaphoreCreateMutex();
  codeQueue = xQueueCreate(8, sizeof(uint8_t[4]));
  feedbackQueue = xQueueCreate(8, sizeof(uint8_t[2]));
  if (!aiBtnSemaphore || !predictionMutex || !codeQueue || !feedbackQueue) {
    Serial.println("Failed to create RTOS objects");
    while (1) delay(1000);
  }

  dealer.setup();

  pinMode(17, OUTPUT);
  digitalWrite(17, HIGH);
  pixel.begin();
  pixel.show();

  pinMode(AI_BUTTON, INPUT_PULLUP);
  attachInterrupt(AI_BUTTON, onAIPredictButton, FALLING);

  NimBLEDevice::init(DEVICE_NAME);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  NimBLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |
    NIMBLE_PROPERTY::NOTIFY
  );
  pCharacteristic->setCallbacks(&chrCallbacks);
  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(DEVICE_NAME);
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->enableScanResponse(true);
  pAdvertising->start();
  Serial.println("Advertising started");

  xTaskCreatePinnedToCore(mlTask, "ML Task", 6144, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(aiTask, "AI Task", 16384, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
