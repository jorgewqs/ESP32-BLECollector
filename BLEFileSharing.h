/*

  ESP32 BLE Collector - A BLE scanner with sqlite data persistence on the SD Card
  Source: https://github.com/tobozo/ESP32-BLECollector

  MIT License

  Copyright (c) 2019 tobozo

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

  -----------------------------------------------------------------------------

*/

#include "BLEDevice.h"
#include "BLEAdvertisedDevice.h"
#include "BLEClient.h"
#include "BLEScan.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#define TICKS_TO_DELAY 1000

static BLEUUID FileSharingServiceUUID( "f59f6622-1540-0001-8d71-362b9e155667" ); // generated UUID for the service
static BLEUUID FileSharingWriteUUID(   "f59f6622-1540-0002-8d71-362b9e155667" ); // characteristic to write file_chunk locally
static BLEUUID FileSharingRouteUUID(   "f59f6622-1540-0003-8d71-362b9e155667" ); // characteristic to manage routing
static BLEUUID timeServiceUUID(        (uint16_t)0x1805 ); // gatt "Current Time Service", "org.bluetooth.service.current_time"
static BLEUUID timeCharacteristicUUID( (uint16_t)0x2a2b ); // gatt "Current Time", "org.bluetooth.characteristic.current_time"

BLEServer*         BLESharingServer;
BLEClient*         BLESharingClient;

BLEService*        BLESharingService;
BLERemoteService*  BLESharingRemoteService;

BLEAdvertising*    BLESharingAdvertising;

BLECharacteristic* FileSharingWriteChar;
BLECharacteristic* FileSharingRouteChar;
BLECharacteristic* TimeServerChar;

BLERemoteCharacteristic* FileSharingReadRemoteChar;
BLERemoteCharacteristic* FileSharingRouterRemoteChar;
BLERemoteCharacteristic* TimeRemoteChar;

BLE2902* BLESharing2902Descriptor;

std::string timeServerBLEAddress;
std::string fileServerBLEAddress;

esp_ble_addr_type_t timeServerClientType;
esp_ble_addr_type_t fileServerClientType;

typedef struct {
  uint16_t year;
  uint8_t  month;
  uint8_t  day;
  uint8_t  hour;
  uint8_t  minutes;
  uint8_t  seconds;
  uint8_t  wday;
  uint8_t  fraction;
  uint8_t  adjust = 0;
  uint8_t  tz;
} bt_time_t;

bt_time_t BLERemoteTime;

static File   FileReceiver;
static int    binary_file_length = 0;
static size_t FileReceiverExpectedSize = 0;
static size_t FileReceiverReceivedSize = 0;
static size_t FileReceiverProgress = 0;

static bool isFileSharingClientConnected = false;
static bool fileSharingServerTaskIsRunning = false;
static bool fileSharingServerTaskShouldStop = false;
static bool fileSharingSendFileError = false;
static bool fileSharingClientTaskIsRunning = false;
static bool fileSharingClientStarted = false;
static bool timeServerIsRunning = false;
static bool timeServerStarted = false;
static bool TimeServerSignalSent = false;
static bool timeClientisRunning = false;
static bool timeClientisStarted = false;
static bool fileSharingEnabled = false;
static bool fileDownloadingEnabled = false;

const char *sizeMarker = "size:";
const char* closeMessage = "close";

// helper
char *substr(char *src, int pos, int len) {
  char* dest = NULL;
  if (len > 0) {
    dest = (char*)calloc(len + 1, 1);
    if (NULL != dest) {
      strncat(dest, src + pos, len);
    }
  }
  return dest;
}


/******************************************************
  BLE Time Client methods
******************************************************/


static void setBLETime() {
  DateTime UTCTime   = DateTime(BLERemoteTime.year, BLERemoteTime.month, BLERemoteTime.day, BLERemoteTime.hour, BLERemoteTime.minutes, BLERemoteTime.seconds);
  DateTime LocalTime = UTCTime.unixtime() + BLERemoteTime.tz * 3600;
  
  dumpTime("UTC:", UTCTime);
  dumpTime("Local:", LocalTime);
  setTime( LocalTime.unixtime() );
  Serial.printf("[Heap: %06d] Time has been set to: %04d-%02d-%02d %02d:%02d:%02d\n",
   freeheap,
   LocalTime.year(),
   LocalTime.month(),
   LocalTime.day(),
   LocalTime.hour(),
   LocalTime.minute(),
   LocalTime.second()
  );
#if HAS_EXTERNAL_RTC
  RTC.adjust(LocalTime);
#endif
  logTimeActivity(SOURCE_BLE, LocalTime.unixtime() );
  lastSyncDateTime = LocalTime;
  HasBTTime = true;
  DayChangeTrigger = true;
  TimeIsSet = true;
  timeHousekeeping();
}


static void TimeClientNotifyCallback( BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify ) {
  //pBLERemoteCharacteristic->getHandle();
  log_w("Received time");
  memcpy( &BLERemoteTime, pData, length );
  setBLETime();
};

class TimeClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pC) {
      log_w("[Heap: %06d] Connect!!", freeheap);
    }
    void onDisconnect(BLEClient* pC) {
      if ( !HasBTTime ) {
        foundTimeServer = false;
        log_w("[Heap: %06d] Disconnect without time!!", freeheap);
        // oh that's dirty
        //ESP.restart();
      } else {
        foundTimeServer = true;
        log_w("[Heap: %06d] Disconnect with time!!", freeheap);
      }
    }
};

TimeClientCallbacks *TimeClientCallback;


static void stopTimeClient() {
  if ( BLESharingClient != NULL ) {
    if ( BLESharingClient->isConnected() ) {
      BLESharingClient->disconnect();
    }
    //delete BLESharingClient; BLESharingClient = NULL;
  }
  /*
    if( TimeRemoteChar != NULL )     { delete TimeRemoteChar; TimeRemoteChar = NULL; }

    if( BLESharingRemoteService != NULL ) { delete BLESharingRemoteService; BLESharingRemoteService = NULL; }
  */
  if ( TimeClientCallback != NULL ) {
    delete TimeClientCallback;
    TimeClientCallback = NULL;
  }
  foundTimeServer = false;
  timeClientisRunning = false;
}


static void TimeClientTask( void * param ) {

  if ( BLESharingClient == NULL ) {
    BLESharingClient = BLEDevice::createClient();
  }
  if ( TimeClientCallback == NULL ) {
    TimeClientCallback = new TimeClientCallbacks();
  }
  BLESharingClient->setClientCallbacks( TimeClientCallback );

  HasBTTime = false;
  log_w("[Heap: %06d] Will connect to address %s", freeheap, timeServerBLEAddress.c_str());
  if ( !BLESharingClient->connect( timeServerBLEAddress, timeServerClientType ) ) {
    log_e("[Heap: %06d] Failed to connect to address %s", freeheap, timeServerBLEAddress.c_str());
    stopTimeClient();
    vTaskDelete( NULL ); return;
  }
  log_w("[Heap: %06d] Connected to address %s", freeheap, timeServerBLEAddress.c_str());
  BLESharingRemoteService = BLESharingClient->getService( timeServiceUUID );
  if (BLESharingRemoteService == nullptr) {
    log_e("Failed to find our service UUID: %s", timeServiceUUID.toString().c_str());
    stopTimeClient();
    vTaskDelete( NULL ); return;
  }
  TimeRemoteChar = BLESharingRemoteService->getCharacteristic( timeCharacteristicUUID );
  if (TimeRemoteChar == nullptr) {
    log_e("Failed to find our characteristic timeCharacteristicUUID: %s, disconnecting", timeCharacteristicUUID.toString().c_str());
    stopTimeClient();
    vTaskDelete( NULL ); return;
  }
  log_w("[Heap: %06d] registering for notification", freeheap);
  TimeRemoteChar->registerForNotify( TimeClientNotifyCallback );
  TickType_t last_wake_time;
  last_wake_time = xTaskGetTickCount();

  while (BLESharingClient->isConnected()) {
    vTaskDelayUntil(&last_wake_time, TICKS_TO_DELAY / portTICK_PERIOD_MS);
    // TODO: max wait time before force exit
    if ( HasBTTime ) {
      break;
    }
  }
  log_w("[Heap: %06d] client disconnected", freeheap);
  stopTimeClient();
  vTaskDelete( NULL );
}




/******************************************************
  BLE Time Server methods
******************************************************/


class TimeServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) {
      TimeServerSignalSent = false;
      BLEDevice::getAdvertising()->stop();
    }
    void onDisconnect(BLEServer *pServer) {
      TimeServerSignalSent = true;
    }
};

TimeServerCallbacks *TimeServerCallback;


static void stopTimeServer() {
  BLESharingAdvertising->stop();
  BLESharingService->stop();
  log_w("BLESharingServer->removeService( BLESharingService )");
  BLESharingServer->removeService( BLESharingService );
  log_w("delete BLESharing2902Descriptor");
  delete BLESharing2902Descriptor; BLESharing2902Descriptor = NULL;
  log_w("delete TimeServerCallback");
  delete TimeServerCallback; TimeServerCallback = NULL;
  log_w("delete TimeServerChar");
  delete TimeServerChar; TimeServerChar = NULL;
  log_w("delete BLESharingServer");
  delete BLESharingServer; BLESharingServer = NULL;
  log_w("delete BLESharingService");
  delete BLESharingService; BLESharingService = NULL;
  timeServerIsRunning = false;
  log_w("Stopped time server");
}


static void TimeServerTaskNotify( void * param ) {
  TickType_t lastWaketime;
  lastWaketime = xTaskGetTickCount();
  DateTime LocalTime = DateTime(year(), month(), day(), hour(), minute(), second());
  DateTime UTCTime   = LocalTime.unixtime() - timeZone * 3600;
  bt_time_t _time;
  //struct timeval tv;
  //struct tm* _t;
  while ( !TimeServerSignalSent ) {
    // because it's not enough maintaining those:
    //   1) internal rtc clock
    //   2) external rtc clock
    //   3) external gps clock
    // let's use the ESP32 recommended example and throw an extra snpm clock ?
    // ... nah, fuck this
    // gettimeofday(&tv, nullptr);
    // _t = localtime(&(tv.tv_sec));
    _time.year     = UTCTime.year();   // 1900 + _t->tm_year;
    _time.month    = UTCTime.month();  // _t->tm_mon + 1;
    _time.wday     = 0;        // _t->tm_wday == 0 ? 7 : _t->tm_wday;
    _time.day      = UTCTime.day();    // _t->tm_mday;
    _time.hour     = UTCTime.hour();   // _t->tm_hour;
    _time.minutes  = UTCTime.minute(); // _t->tm_min;
    _time.seconds  = UTCTime.second(); // _t->tm_sec;
    _time.fraction = 0;        // tv.tv_usec * 256 / 1000000;
    _time.tz       = timeZone; // wat
    TimeServerChar->setValue((uint8_t*)&_time, sizeof(bt_time_t));
    TimeServerChar->notify();
    // send notification with date/time exactly every TICKS_TO_DELAY ms
    vTaskDelayUntil(&lastWaketime, TICKS_TO_DELAY / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}



static void TimeServerTask( void * param ) {
  Serial.println("Starting BLE Time Server");

  BLEDevice::setMTU(50);

  BLESharing2902Descriptor = new BLE2902();

  TimeServerCallback = new TimeServerCallbacks();

  if ( BLESharingAdvertising == NULL ) {
    BLESharingAdvertising = BLEDevice::getAdvertising();
  }
  BLESharingServer = BLEDevice::createServer();

  BLESharingServer->setCallbacks( TimeServerCallback );
  
  BLESharingService = BLESharingServer->createService( timeServiceUUID );

  TimeServerChar = BLESharingService->createCharacteristic(
    timeCharacteristicUUID,
    BLECharacteristic::PROPERTY_NOTIFY   |
    BLECharacteristic::PROPERTY_READ
  );

  BLESharing2902Descriptor->setNotifications( true );
  TimeServerChar->addDescriptor( BLESharing2902Descriptor );

  BLESharingService->start();

  BLESharingAdvertising->addServiceUUID( timeServiceUUID );
  BLESharingAdvertising->setMinInterval( 0x100 );
  BLESharingAdvertising->setMaxInterval( 0x200 );
  log_w("Starting advertising");
  BLEDevice::startAdvertising();
  log_w("TimeServer Advertising started");
  
  TimeServerSignalSent = false;

  xTaskCreate( TimeServerTaskNotify, "TimeServerTaskNotify", 2048, NULL, 6, NULL );

  while ( timeServerIsRunning ) {
    if( TimeServerSignalSent ) {
      break;
    } else {
      vTaskDelay(10);
    }
  }
  stopTimeServer();
  vTaskDelete( NULL );
}





/******************************************************
  BLE File Receiver Methods
******************************************************/

void FileSharingReceiveFile( const char* filename ) {
  FileReceiverReceivedSize = 0;
  FileReceiverProgress = 0;
  FileReceiver = BLE_FS.open( filename, FILE_WRITE );
  if ( !FileReceiver ) {
    log_e("Failed to create %s", filename);
  }
  log_v("Successfully opened %s for writing", filename);
}


void FileSharingCloseFile() {
  if ( !FileReceiver ) {
    log_e("Nothing to close!");
    return;
  }
  takeMuxSemaphore();
  FileReceiver.close();
  if ( FileReceiverReceivedSize != FileReceiverExpectedSize ) {
    log_e("Total size != expected size ( %d != %d )", FileReceiver.size(), FileReceiverExpectedSize);
    Out.println( "Copy Failed, please try again." );
  } else {
    Out.println( "Copy successful!" );
  }
  giveMuxSemaphore();
  //TODO: sha256_sum
  FileReceiverExpectedSize = 0;
  FileReceiverReceivedSize = 0;
  FileReceiverProgress = 0;
}


class FileSharingWriteCallbacks : public BLECharacteristicCallbacks {
    void onWrite( BLECharacteristic* WriterAgent ) {
      size_t len = WriterAgent->getDataLength();
      if ( FileReceiverExpectedSize == 0 ) {
        // no size was previously sent, can't calculate
        log_e("Ignored %d bytes", len);
        return;
      }
      size_t progress = (((float)FileReceiverReceivedSize / (float)FileReceiverExpectedSize) * 100.00);
      if ( FileReceiver ) {
        FileReceiver.write( WriterAgent->getData(), len );
        log_w("Wrote %d bytes", len);
        FileReceiverReceivedSize += len;
      } else {
        // file write problem ?
        log_e("Ignored %d bytes", len);
      }
      if ( FileReceiverProgress != progress ) {
        takeMuxSemaphore();
        UI.PrintProgressBar( (Out.width * progress) / 100 );
        giveMuxSemaphore();
        FileReceiverProgress = progress;
        vTaskDelay(10);
      }
    }
};


class FileSharingRouteCallbacks : public BLECharacteristicCallbacks {
    void onWrite( BLECharacteristic* RouterAgent ) {
      char routing[512] = {0};
      memcpy( &routing, RouterAgent->getData(), RouterAgent->getDataLength() );
      log_v("Received copy routing query: %s", routing);
      if ( strstr(routing, sizeMarker) ) { // messages starting with "size:"
        if ( strlen( routing ) > strlen( sizeMarker ) ) {
          char* lenStr = substr( routing, strlen(sizeMarker), strlen(routing) - strlen(sizeMarker) );
          FileReceiverExpectedSize = atoi( lenStr );
          log_v( "Assigned size_t %d", FileReceiverExpectedSize );
        }
      } else if ( strcmp( routing, closeMessage ) == 0 ) { // file end
        FileSharingCloseFile();
      } else { // filenames
        if ( strcmp( routing, "/" BLE_VENDOR_NAMES_DB_FILE ) == 0 ) {
          FileSharingReceiveFile( "/" BLE_VENDOR_NAMES_DB_FILE );
        }
        if ( strcmp( routing, "/" MAC_OUI_NAMES_DB_FILE ) == 0 ) {
          FileSharingReceiveFile( "/" MAC_OUI_NAMES_DB_FILE);
        }
        takeMuxSemaphore();
        UI.PrintProgressBar( 0 );
        giveMuxSemaphore();
      }
      takeMuxSemaphore();
      Out.println( routing );
      Out.println();
      giveMuxSemaphore();

    }
};


class FileSharingCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* SharingServer,  esp_ble_gatts_cb_param_t *param) {
      log_v("A client is connected, stopping advertising");
      isFileSharingClientConnected = true;
      UI.headerStats("Connected :-)");
      takeMuxSemaphore();
      Out.println( "Client connected!" );
      Out.println();
      giveMuxSemaphore();
      BLEDevice::getAdvertising()->stop();
      // do some voodo on remote MTU for transfert perfs (thanks @chegewara)
      esp_ble_gap_set_prefer_conn_params(param->connect.remote_bda, 6, 6, 0, 500);
    }
    void onDisconnect(BLEServer* SharingServer) {
      log_v("A client disconnected, restarting advertising");
      isFileSharingClientConnected = false;
      UI.headerStats("Advertising (_x_)");
      takeMuxSemaphore();
      Out.println( "Client disconnected" );
      Out.println();
      giveMuxSemaphore();
      //BLEDevice::startAdvertising();
      fileSharingServerTaskShouldStop = true;
    }
};


FileSharingCallbacks *FileSharingCallback;
FileSharingRouteCallbacks *FileSharingRouteCallback;
FileSharingWriteCallbacks *FileSharingWriteCallback;

void stopFileSharingServer() {
  if ( FileSharingCallback != NULL )      {
    delete( FileSharingCallback);
    FileSharingCallback      = NULL;
  }
  if ( FileSharingRouteCallback != NULL ) {
    delete( FileSharingRouteCallback);
    FileSharingRouteCallback = NULL;
  }
  if ( FileSharingWriteCallback != NULL ) {
    delete( FileSharingWriteCallback);
    FileSharingWriteCallback = NULL;
  }
  if ( BLESharing2902Descriptor != NULL ) {
    delete( BLESharing2902Descriptor);
    BLESharing2902Descriptor = NULL;
  }
  fileSharingServerTaskIsRunning = false;
  fileSharingServerTaskShouldStop = false;
  fileDownloadingEnabled = false;
}

// server as a slave service: wait for an upload signal
static void FileSharingServerTask(void* p) {

  BLEDevice::setMTU(517);
  if ( BLESharingAdvertising == NULL ) {
    BLESharingAdvertising = BLEDevice::getAdvertising();
  }
  if ( FileSharingCallback == NULL ) {
    FileSharingCallback = new FileSharingCallbacks();
  }
  if ( FileSharingRouteCallback == NULL ) {
    FileSharingRouteCallback = new FileSharingRouteCallbacks();
  }
  if ( FileSharingWriteCallback == NULL ) {
    FileSharingWriteCallback = new FileSharingWriteCallbacks();
  }
  if ( BLESharing2902Descriptor == NULL ) {
    BLESharing2902Descriptor = new BLE2902();
  }
  if ( BLESharingServer == NULL ) {
    BLESharingServer = BLEDevice::createServer();
  }
  BLESharingServer->setCallbacks( FileSharingCallback );
  BLESharingService = BLESharingServer->createService( FileSharingServiceUUID );

  FileSharingWriteChar = BLESharingService->createCharacteristic(
    FileSharingWriteUUID,
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  FileSharingRouteChar = BLESharingService->createCharacteristic(
    FileSharingRouteUUID,
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_READ   |
    BLECharacteristic::PROPERTY_WRITE
  );

  FileSharingRouteChar->setCallbacks( FileSharingRouteCallback );
  FileSharingWriteChar->setCallbacks( FileSharingWriteCallback );

  BLESharing2902Descriptor->setNotifications(true);
  FileSharingRouteChar->addDescriptor( BLESharing2902Descriptor );

  BLESharingService->start();

  BLESharingAdvertising->addServiceUUID( FileSharingServiceUUID );
  BLESharingAdvertising->setMinInterval(0x100);
  BLESharingAdvertising->setMaxInterval(0x200);

  BLEDevice::startAdvertising();

  Serial.println("FileSharingClientTask up an advertising");
  UI.headerStats("Advertising (_x_)");
  takeMuxSemaphore();
  Out.println();
  Out.println( "Waiting for a BLE peer to send the files" );
  Out.println();
  giveMuxSemaphore();

  while (1) {
    if ( fileSharingServerTaskShouldStop ) { // stop signal from outside the task
      BLESharingAdvertising->stop();
      BLESharingService->stop();
      BLESharingServer->removeService( BLESharingService );
      stopFileSharingServer();
      vTaskDelete( NULL );
    } else {
      vTaskDelay(100);
    }
  }
} // FileSharingServerTask








/******************************************************
  BLE File Sender Methods
******************************************************/

void FileSharingSendFile( const char* filename ) {
  fileSharingSendFileError = false;
  File fileToTransfert = BLE_FS.open( filename );

  if ( !fileToTransfert ) {
    log_e("Can't open %s for reading", filename);
    fileSharingSendFileError = true;
    return;
  }
  size_t totalsize = fileToTransfert.size();
  size_t progress = totalsize;

  // send file size as string
  char myTotalSize[32];
  sprintf( myTotalSize, "%s%d", sizeMarker, totalsize );
  FileSharingRouterRemoteChar->writeValue((uint8_t*)myTotalSize, strlen(myTotalSize), false);

  #define BLE_FILECOPY_BUFFSIZE 512
  uint8_t buff[BLE_FILECOPY_BUFFSIZE];
  uint32_t len = fileToTransfert.read( buff, BLE_FILECOPY_BUFFSIZE );
  log_v("Starting transfert...");
  UI.headerStats(filename);
  takeMuxSemaphore();
  UI.PrintProgressBar( 0 );
  giveMuxSemaphore();
  int lastpercent = 0;
  while ( len > 0 ) {
    progress -= len;
    int percent = 100 - (((float)progress / (float)totalsize) * 100.00);
    if ( !FileSharingReadRemoteChar->writeValue((uint8_t*)&buff, len, false) ) {
      // transfert failed !
      log_e("Failed to send %d bytes (%d percent done) %d / %d", len, percent, progress, totalsize);
      fileSharingSendFileError = true;
      break;
    } else {
      log_v("SUCCESS sending %d bytes (%d percent done) %d / %d", len, percent, progress, totalsize);
    }
    if ( lastpercent != percent ) {
      takeMuxSemaphore();
      UI.PrintProgressBar( (Out.width * percent) / 100 );
      giveMuxSemaphore();
      lastpercent = percent;
      vTaskDelay(10);
    }
    len = fileToTransfert.read( buff, BLE_FILECOPY_BUFFSIZE );
    vTaskDelay(10);
  }
  takeMuxSemaphore();
  UI.PrintProgressBar( Out.width );

  giveMuxSemaphore();

  UI.headerStats("[OK]");
  log_v("Transfert finished!");
  fileToTransfert.close();
}


static void FileSharingRouterCallbacks( BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify ) {
  char routing[512] = {0};
  memcpy( &routing, pData, length );
  log_v("Received routing query: %s", routing);
  if (strcmp(routing, "/" BLE_VENDOR_NAMES_DB_FILE) == 0) {
    FileSharingSendFile( "/" BLE_VENDOR_NAMES_DB_FILE );
  }
  if (strcmp(routing, "/" MAC_OUI_NAMES_DB_FILE) == 0) {
    FileSharingSendFile( "/" MAC_OUI_NAMES_DB_FILE );
  }
};


class FileSharingClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pC) {
      log_v("[Heap: %06d] Connect!!", freeheap);
    }
    void onDisconnect(BLEClient* pC) {
      log_v("[Heap: %06d] Disconnect!!", freeheap);
    }
};

FileSharingClientCallbacks* FileSharingClientCallback;


void stopFileSharingClient() {
  if ( BLESharingClient != NULL ) {
    if ( BLESharingClient->isConnected() ) {
      BLESharingClient->disconnect();
    }
  }
  if ( FileSharingClientCallback != NULL ) {
    log_w("Deleting FileSharingClientCallback");
    delete FileSharingClientCallback; FileSharingClientCallback = NULL;
  }
  fileSharingClientTaskIsRunning = false;
  fileSharingClientStarted = false;
}


static void FileSharingClientTask( void * param ) {

  if ( FileSharingClientCallback == NULL ) {
    FileSharingClientCallback = new FileSharingClientCallbacks();
  }

  BLEDevice::setMTU(517);

  if ( BLESharingClient == NULL ) {
    BLESharingClient  = BLEDevice::createClient();
  }

  BLESharingClient->setClientCallbacks( FileSharingClientCallback );

  //HasBTTime = false;
  log_v("[Heap: %06d] Will connect to address %s", freeheap, fileServerBLEAddress.c_str());
  if ( !BLESharingClient->connect( fileServerBLEAddress, fileServerClientType ) ) {
    log_e("[Heap: %06d] Failed to connect to address %s", freeheap, fileServerBLEAddress.c_str());
    UI.headerStats("Connect failed :-(");
    stopFileSharingClient();
    vTaskDelete( NULL );
    return;
  }
  log_w("[Heap: %06d] Connected to address %s", freeheap, fileServerBLEAddress.c_str());
  BLESharingRemoteService = BLESharingClient->getService( FileSharingServiceUUID );
  if (BLESharingRemoteService == nullptr) {
    log_e("Failed to find our FileSharingServiceUUID: %s", FileSharingServiceUUID.toString().c_str());
    BLESharingClient->disconnect();
    UI.headerStats("Bounding failed :-(");
    stopFileSharingClient();
    vTaskDelete( NULL );
    return;
  }
  FileSharingReadRemoteChar = BLESharingRemoteService->getCharacteristic( FileSharingWriteUUID );
  if (FileSharingReadRemoteChar == nullptr) {
    log_e("Failed to find our characteristic FileSharingWriteUUID: %s, disconnecting", FileSharingWriteUUID.toString().c_str());
    BLESharingClient->disconnect();
    UI.headerStats("Bad char. :-(");
    stopFileSharingClient();
    vTaskDelete( NULL );
    return;
  }

  FileSharingRouterRemoteChar = BLESharingRemoteService->getCharacteristic( FileSharingRouteUUID );
  if (FileSharingRouterRemoteChar == nullptr) {
    log_e("Failed to find our characteristic FileSharingRouteUUID: %s, disconnecting", FileSharingRouteUUID.toString().c_str());
    BLESharingClient->disconnect();
    UI.headerStats("Bad char. :-(");
    stopFileSharingClient();
    vTaskDelete( NULL );
    return;
  }
  FileSharingRouterRemoteChar->registerForNotify( FileSharingRouterCallbacks );

  UI.headerStats("Connected :-)");

  const char* BLEFileToSend = "/" BLE_VENDOR_NAMES_DB_FILE;
  const char* MACFileToSend = "/" MAC_OUI_NAMES_DB_FILE;

  if ( FileSharingRouterRemoteChar->writeValue((uint8_t*)BLEFileToSend, strlen(BLEFileToSend), true) ) {
    UI.headerStats("Discussing :-)");
    log_v("Will start sending %s file", BLEFileToSend );
    FileSharingSendFile( BLEFileToSend );
    if ( !fileSharingSendFileError && FileSharingRouterRemoteChar->writeValue((uint8_t*)closeMessage, strlen(closeMessage), true) ) {
      log_v("Successfully sent bytes from %s file", BLEFileToSend );
      UI.headerStats("Copy complete :-)");
    } else {
      log_e("COPY ERROR FOR %s file", BLEFileToSend );
      UI.headerStats("Copy error :-(");
    }
  }

  if ( !fileSharingSendFileError && FileSharingRouterRemoteChar->writeValue((uint8_t*)MACFileToSend, strlen(MACFileToSend), true) ) {
    UI.headerStats("Discussing :-)");
    log_v("Will start sending %s file", MACFileToSend );
    FileSharingSendFile( MACFileToSend );
    if ( !fileSharingSendFileError && FileSharingRouterRemoteChar->writeValue((uint8_t*)closeMessage, strlen(closeMessage), true) ) {
      log_v("Successfully sent bytes from %s file", MACFileToSend );
      UI.headerStats("Copy complete :-)");
    } else {
      log_e("COPY ERROR FOR %s file", MACFileToSend );
      UI.headerStats("Copy error :-(");
    }
  } else {
    log_e("Skipping %s because previous errors", MACFileToSend );
  }

  stopFileSharingClient();
  log_v("Deleting FileSharingClientTask" );
  vTaskDelete( NULL );

} // FileSharingClientTask