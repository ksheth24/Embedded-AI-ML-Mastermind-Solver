/**
 * @file NimBLE_Server_Switching.ino
 * @author H2zero
 * @author Jason Hsiao
 * @author STUDENT NAME HERE
 * @date 2/2/2025
 * @version 1.0
 *
 * @brief Starter Code for the switching BLE Client portion of ECE4180 Lab 3
 * (Part 5)
 * @see Course website, Canvas, GitHub or PowerPoint slides for more details
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

#define CODEMAKER
#include <ECE4180MasterMind.h>

// Matches the Player's UUIDs
#define SERVICE_UUID "2006"
#define CHAR_UUID "0001"  // Ensure Player uses this exact string or a hex UUID

PlayerBuffer currentRow;
CodeMaker host;

bool isConnected = false;
bool waitingForFeedback = false;  // Is it the Dealer's turn to type?

static const NimBLEAdvertisedDevice* advDevice;
static bool doConnect = false;
static uint32_t scanTimeMs = 5000; /** scan time in milliseconds, 0 = scan forever */

/** Now we can read/write/subscribe the characteristics of the services we are interested in */
NimBLERemoteService* pSvc = nullptr;
NimBLERemoteCharacteristic* pChr = nullptr;
NimBLERemoteDescriptor* pDsc = nullptr;

struct Player {
    NimBLEClient* pClient = nullptr;
    NimBLERemoteCharacteristic* pChar = nullptr;
    bool connected = false;
    int points = 0;
    bool turn = false;
};

Player players[1];
int connectedCount = 0;

// Global flags for the state machine
static NimBLEAddress addrToConnect;

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    Serial.printf("Connected\n");
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    Serial.printf("%s Disconnected, reason = %d - Starting scan\n", pClient->getPeerAddress().toString().c_str(), reason);
    NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    doConnect = true;
  }

} clientCallbacks;

bool connectToServer(NimBLEAddress addr) {
    if (connectedCount >= 3) return false;

    Serial.printf("Connecting to %s...\n", addr.toString().c_str());
    NimBLEClient* pClient = NimBLEDevice::createClient();

    //======================================//
    // TODO: USING THE pClient, connect and //
    // then add the pointer to connected    //
    // device array for future use          //
    // Follow the previous starter code     //
    //======================================//
    if (!pClient) {
        Serial.println("Failed to create client");
        return false;
    }

    pClient->setClientCallbacks(&clientCallbacks, false);
    pClient->setConnectionParams(12, 12, 0, 150);
    pClient->setConnectTimeout(5000);

    if (!pClient->connect(addr)) {
        NimBLEDevice::deleteClient(pClient);
        Serial.println("Failed to connect");
        return false;
    }

    Serial.printf("Connected to: %s RSSI: %d\n",
        pClient->getPeerAddress().toString().c_str(),
        pClient->getRssi());

    pSvc = pClient->getService(SERVICE_UUID);
    if (!pSvc) {
        Serial.println("Service not found");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    pChr = pSvc->getCharacteristic(CHAR_UUID);
    if (!pChr) {
        Serial.println("Characteristic not found");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    if (!pChr->subscribe(true, handleMove)) {
        Serial.println("Failed to subscribe to notifications");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    players[connectedCount].pClient = pClient;
    players[connectedCount].pChar = pChr;
    players[connectedCount].connected = true;

    Serial.printf("Player %d registered\n", connectedCount);
    connectedCount++;

    Serial.println("Ready!");

    return true;
}

/** Define a class to handle the callbacks when scan events are received */
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (advertisedDevice->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
        
        // Check if we already connected to this MAC address
        for (int i = 0; i < connectedCount; i++) {
            if (players[i].pClient->getPeerAddress().equals(advertisedDevice->getAddress())) {
                return; // Already have this player
            }
        }

        Serial.println("Found a new Mastermind Player!");
        NimBLEDevice::getScan()->stop(); // Stop immediately
        addrToConnect = advertisedDevice->getAddress();
        doConnect = true;
    }
  }

  /** Callback to process the results of the completed scan or restart it */
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    Serial.printf("Scan Ended, reason: %d, device count: %d; Restarting scan\n", reason, results.getCount());
    NimBLEDevice::getScan()->start(scanTimeMs, false, true);
  }
} scanCallbacks;

/** Notification / Indication receiving handler callback */
void handleMove(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  Serial.println("Notification received");

    uint8_t playersGuess[4];

    for (int i = 0; i < 4; i++) {
        playersGuess[i] = pData[i];
    }

    Serial.printf("Guess: %d %d %d %d\n",
                  playersGuess[0],
                  playersGuess[1],
                  playersGuess[2],
                  playersGuess[3]);

    uint8_t results[2];

    host.checkGuess(results, playersGuess);

    Serial.printf("Results -> Black: %d White: %d\n",
                  results[0], results[1]);
    players[0].points += results[0] * 5 +  results[1];
    Serial.printf("Points: %d\n", players[0].points);

    //======================================//
    // TODO: Copy and paste working handler //
    // If you did previous part right, it   //
    // should be exactly the same           //
    //======================================//
    pRemoteCharacteristic->writeValue(results);
    pChr->canNotify();
    if (results[0] == 4) {
        host.endGame();
    }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Starting NimBLE Client\n");

  /** Initialize NimBLE and set the device name */
  NimBLEDevice::init("NimBLE-Client");

  /** Optional: set the transmit power */
  NimBLEDevice::setPower(3); /** 3dbm */
  NimBLEScan* pScan = NimBLEDevice::getScan();

  /** Set the callbacks to call when scan events occur, no duplicates */
  pScan->setScanCallbacks(&scanCallbacks, false);

  /** Set scan interval (how often) and window (how long) in milliseconds */
  pScan->setInterval(100);
  pScan->setWindow(100);

  /**
     * Active scan will gather scan response data from advertisers
     *  but will use more energy from both devices
     */
  pScan->setActiveScan(true);

  /** Start scanning for advertisers */
  pScan->start(scanTimeMs);
  Serial.printf("Scanning for peripherals\n");

  //======================================//
  // TODO: Set-up your code-breaker here  //
  //======================================//
  host.setup();
  host.generateBiasedCode(static_cast<uint>(random(0,3))); 
}

void loop() {
    if (doConnect) {
        doConnect = false;
        
        if (connectToServer(addrToConnect)) {
            Serial.printf("Player %d ready!\n", connectedCount);
        } else {
            Serial.println("Connection failed. Retrying scan...");
        }

        // Delay to let the radio "settle" before scanning again
        delay(500);

        if (connectedCount < 3) {
            NimBLEDevice::getScan()->start(0, false, true);
        } else {
            Serial.println("All players connected. Game on!");
        }
    }
    
    delay(10);
}
