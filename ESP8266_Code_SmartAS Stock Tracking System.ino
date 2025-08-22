#include <ESP8266WiFi.h>
#include <FirebaseESP8266.h>
#include <SPI.h>
#include <MFRC522.h>

// WiFi Configuration
#define WIFI_SSID "AMEERSATHIQ 5345"
#define WIFI_PASSWORD "6m8,Y663"

// Firebase Configuration
#define FIREBASE_HOST "smartas-b8f3b-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "AIzaSyA8UiGYqFZoZteypTXHFnV-ghxKkaYOl-Y"

// RFID Reader Configuration
#define ROOM_A_SS_PIN D1    // GPIO5
#define ROOM_A_RST_PIN D3   // GPIO0
#define ROOM_B_SS_PIN D2    // GPIO4
#define ROOM_B_RST_PIN D8   // GPIO15

// Timeout configuration (10 minutes in milliseconds)
#define TRANSFER_TIMEOUT 600000

// Create RFID instances
MFRC522 roomAReader(ROOM_A_SS_PIN, ROOM_A_RST_PIN);
MFRC522 roomBReader(ROOM_B_SS_PIN, ROOM_B_RST_PIN);

// Firebase objects
FirebaseData fbData;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;

// Stock tracking structure
struct StockItem {
  String id;
  unsigned long roomAScanTime;
  unsigned long roomBScanTime;
  bool inRoomA;
  bool inRoomB;
  bool missing;
};

// Global variables
StockItem currentItems[20];  // Adjust size based on expected concurrent items
int itemCount = 0;
unsigned long lastCheckTime = 0;

void setup() {
  Serial.begin(115200);
  
  // Initialize SPI and RFID readers
  SPI.begin();
  roomAReader.PCD_Init();
  roomBReader.PCD_Init();
  
  // Initialize WiFi
  initializeWiFi();
  
  // Initialize Firebase
  initializeFirebase();
  
  Serial.println("\n=== RFID Stock Tracking System Initialized ===");
  Serial.println("Room A and Room B RFID readers are ready");
  Serial.println("Waiting for stock items...\n");
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Check for RFID scans in both rooms
  checkRFIDReaders();
  
  // Check for missing items every second
  if (currentMillis - lastCheckTime >= 1000) {
    lastCheckTime = currentMillis;
    checkForMissingItems();
  }
  
  // Small delay to prevent watchdog trigger
  delay(10);
}

void initializeWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  
  Serial.print("\nConnected! IP: ");
  Serial.println(WiFi.localIP());
}

void initializeFirebase() {
  fbConfig.host = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
}

void checkRFIDReaders() {
  // Check Room A RFID reader
  if (roomAReader.PICC_IsNewCardPresent() && roomAReader.PICC_ReadCardSerial()) {
    String stockID = getRFIDUID(roomAReader);
    processRoomAScan(stockID);
    roomAReader.PICC_HaltA();
  }
  
  // Check Room B RFID reader
  if (roomBReader.PICC_IsNewCardPresent() && roomBReader.PICC_ReadCardSerial()) {
    String stockID = getRFIDUID(roomBReader);
    processRoomBScan(stockID);
    roomBReader.PICC_HaltA();
  }
}

String getRFIDUID(MFRC522 &reader) {
  String uidString = "";
  for (byte i = 0; i < reader.uid.size; i++) {
    uidString += String(reader.uid.uidByte[i] < 0x10 ? "0" : "");
    uidString += String(reader.uid.uidByte[i], HEX);
  }
  uidString.toUpperCase();
  return uidString;
}

void processRoomAScan(String stockID) {
  unsigned long currentTime = millis();
  Serial.println("Room A Scan: " + stockID + " at " + String(currentTime));
  
  // Check if this item is already being tracked
  int itemIndex = findStockItem(stockID);
  
  if (itemIndex == -1) {
    // New item detected in Room A
    if (itemCount < 20) {
      currentItems[itemCount].id = stockID;
      currentItems[itemCount].roomAScanTime = currentTime;
      currentItems[itemCount].roomBScanTime = 0;
      currentItems[itemCount].inRoomA = true;
      currentItems[itemCount].inRoomB = false;
      currentItems[itemCount].missing = false;
      itemCount++;
      
      // Update Firebase
      updateFirebase(stockID, "RoomA_Input", currentTime);
      Serial.println("New item registered in Room A");
    } else {
      Serial.println("Error: Maximum item capacity reached");
    }
  } else {
    // Existing item - check if it's being dispatched from Room A
    if (currentItems[itemIndex].inRoomA && !currentItems[itemIndex].inRoomB) {
      currentItems[itemIndex].roomAScanTime = currentTime;
      currentItems[itemIndex].inRoomA = false; // Mark as dispatched from Room A
      
      // Update Firebase
      updateFirebase(stockID, "RoomA_Dispatch", currentTime);
      Serial.println("Item dispatched from Room A");
    } else if (!currentItems[itemIndex].inRoomA && currentItems[itemIndex].inRoomB) {
      // Item returning to Room A from Room B (if needed)
      currentItems[itemIndex].roomAScanTime = currentTime;
      currentItems[itemIndex].inRoomA = true;
      
      // Update Firebase
      updateFirebase(stockID, "RoomA_Return", currentTime);
      Serial.println("Item returned to Room A");
    }
  }
}

void processRoomBScan(String stockID) {
  unsigned long currentTime = millis();
  Serial.println("Room B Scan: " + stockID + " at " + String(currentTime));
  
  // Check if this item is already being tracked
  int itemIndex = findStockItem(stockID);
  
  if (itemIndex == -1) {
    // New item detected in Room B (directly arrived without Room A scan)
    if (itemCount < 20) {
      currentItems[itemCount].id = stockID;
      currentItems[itemCount].roomAScanTime = 0;
      currentItems[itemCount].roomBScanTime = currentTime;
      currentItems[itemCount].inRoomA = false;
      currentItems[itemCount].inRoomB = true;
      currentItems[itemCount].missing = false;
      itemCount++;
      
      // Update Firebase
      updateFirebase(stockID, "RoomB_Input_Direct", currentTime);
      Serial.println("New item registered directly in Room B");
    } else {
      Serial.println("Error: Maximum item capacity reached");
    }
  } else {
    // Existing item - check if it's arriving at Room B
    if (!currentItems[itemIndex].inRoomA && !currentItems[itemIndex].inRoomB) {
      // Item was dispatched from Room A and now arriving at Room B
      currentItems[itemIndex].roomBScanTime = currentTime;
      currentItems[itemIndex].inRoomB = true;
      currentItems[itemIndex].missing = false; // Item found, not missing
      
      // Calculate transfer time
      unsigned long transferTime = currentTime - currentItems[itemIndex].roomAScanTime;
      
      // Update Firebase
      updateFirebase(stockID, "RoomB_Input", currentTime);
      Serial.println("Item arrived at Room B. Transfer time: " + String(transferTime) + "ms");
      
      // Check if transfer was within timeout
      if (transferTime > TRANSFER_TIMEOUT) {
        Serial.println("Warning: Transfer exceeded timeout period");
        updateFirebase(stockID, "Transfer_Delayed", currentTime);
      }
    } else if (currentItems[itemIndex].inRoomB) {
      // Item being dispatched from Room B
      currentItems[itemIndex].roomBScanTime = currentTime;
      currentItems[itemIndex].inRoomB = false;
      
      // Update Firebase
      updateFirebase(stockID, "RoomB_Dispatch", currentTime);
      Serial.println("Item dispatched from Room B");
      
      // Remove item from tracking if it's leaving Room B
      removeStockItem(itemIndex);
    }
  }
}

int findStockItem(String stockID) {
  for (int i = 0; i < itemCount; i++) {
    if (currentItems[i].id == stockID) {
      return i;
    }
  }
  return -1; // Not found
}

void removeStockItem(int index) {
  if (index < 0 || index >= itemCount) return;
  
  // Shift all items after the removed one
  for (int i = index; i < itemCount - 1; i++) {
    currentItems[i] = currentItems[i + 1];
  }
  
  itemCount--;
}

void checkForMissingItems() {
  unsigned long currentTime = millis();
  
  for (int i = 0; i < itemCount; i++) {
    // Check if item was dispatched from Room A but hasn't arrived at Room B
    if (!currentItems[i].inRoomA && !currentItems[i].inRoomB && 
        !currentItems[i].missing && 
        currentItems[i].roomAScanTime > 0) {
      
      unsigned long timeSinceDispatch = currentTime - currentItems[i].roomAScanTime;
      
      if (timeSinceDispatch > TRANSFER_TIMEOUT) {
        // Item is missing
        currentItems[i].missing = true;
        
        // Update Firebase
        updateFirebase(currentItems[i].id, "Missing", currentTime);
        Serial.println("ALERT: Item " + currentItems[i].id + " is missing!");
      }
    }
  }
}

void updateFirebase(String stockID, String eventType, unsigned long timestamp) {
  String path = "/stockTracking/" + stockID + "/" + eventType;
  
  // Convert timestamp to human-readable format if needed
  String timestampStr = String(timestamp);
  
  if (Firebase.setString(fbData, path, timestampStr)) {
    Serial.println("Firebase updated: " + path + " = " + timestampStr);
  } else {
    Serial.println("Firebase error: " + fbData.errorReason());
  }
  
  // Also update the latest event for this stock ID
  String latestPath = "/stockTracking/" + stockID + "/latestEvent";
  String eventData = eventType + ":" + timestampStr;
  
  if (Firebase.setString(fbData, latestPath, eventData)) {
    // Success
  } else {
    Serial.println("Firebase error updating latest event: " + fbData.errorReason());
  }
}