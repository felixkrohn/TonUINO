#include <DFMiniMp3.h>
#include <EEPROM.h>
#include <JC_Button.h>
#include <MFRC522.h>
#include <SPI.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>

/*
   _____         _____ _____ _____ _____
  |_   _|___ ___|  |  |     |   | |     |
    | | | . |   |  |  |-   -| | | |  |  |
    |_| |___|_|_|_____|_____|_|___|_____|
    TonUINO Version 2.1 + McGreg changes v0.1

    created by Thorsten Voß and licensed under GNU/GPL.
    Information and contribution at https://tonuino.de.
*/
/*
 * button 1: select
 * button 2: up
 * button 3: down
 * button 4: admin menu
 * button 5: play next track, reset/reboot on long press
 * 
 * play mode: whole directory, remember last position as long as card is not changed
*/

static const uint32_t cardCookie = 322417479;

// DFPlayer Mini
SoftwareSerial mySoftwareSerial(2, 3); // RX, TX
uint16_t numTracksInFolder;
uint16_t currentTrack;
uint8_t queue[255];

struct folderSettings {
  uint8_t folder;
};

// this object stores nfc tag data
struct nfcTagObject {
  uint32_t cookie;
  uint8_t version;
  folderSettings nfcFolderSettings;
  uint8_t folder;
};

// admin settings stored in eeprom
struct adminSettings {
  uint32_t cookie;
  byte version;
  bool locked;
  long standbyTimer;
  uint8_t adminMenuLocked;
  bool stopWhenCardAway;
};


//StopWhenCardAway settings
static bool hasCard = false;
static byte lastCardUid[4];
static byte retries;
static bool lastCardWasUL;
static bool forgetLastCard=false;

const byte PCS_NO_CHANGE     = 0; // no change detected since last pollCard() call
const byte PCS_NEW_CARD      = 1; // card with new UID detected (had no card or other card before)
const byte PCS_CARD_GONE     = 2; // card is not reachable anymore
const byte PCS_CARD_IS_BACK  = 3; // card was gone, and is now back again

adminSettings mySettings;
nfcTagObject myCard;
folderSettings *myFolder;
unsigned long sleepAtMillis = 0;
static uint16_t _lastTrackFinished;


static void nextTrack(uint16_t track);
uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview = false, int previewFromFolder = 0, int defaultValue = 0, bool exitWithLongPress = false);
bool isPlaying();
void writeCard(nfcTagObject nfcTag);
void dump_byte_array(byte * buffer, byte bufferSize);
void adminMenu(bool fromCard = false);
bool knownCard = false;

// implement a notification class,
// its member methods will get called
//
class Mp3Notify {
  public:
    static void OnError(uint16_t errorCode) {
      // see DfMp3_Error for code meaning
      Serial.println();
      Serial.print("Com Error ");
      Serial.println(errorCode);
    }
    static void PrintlnSourceAction(DfMp3_PlaySources source, const char* action) {
      if (source & DfMp3_PlaySources_Sd) Serial.print("SD Karte ");
      if (source & DfMp3_PlaySources_Usb) Serial.print("USB ");
      if (source & DfMp3_PlaySources_Flash) Serial.print("Flash ");
      Serial.println(action);
    }
    static void OnPlayFinished(DfMp3_PlaySources source, uint16_t track) {
      Serial.print("Track ended");
      Serial.println(track);
      delay(100);
      nextTrack(track);
    }
    static void OnPlaySourceOnline(DfMp3_PlaySources source) {
      PrintlnSourceAction(source, "online");
    }
    static void OnPlaySourceInserted(DfMp3_PlaySources source) {
      PrintlnSourceAction(source, "ready");
    }
    static void OnPlaySourceRemoved(DfMp3_PlaySources source) {
      PrintlnSourceAction(source, "removed");
    }
};

static DFMiniMp3<SoftwareSerial, Mp3Notify> mp3(mySoftwareSerial);

void writeSettingsToFlash() {
  Serial.println(F("=== writeSettingsToFlash()"));
  int address = sizeof(myFolder->folder) * 100;
  EEPROM.put(address, mySettings);
}

void resetSettings() {
  Serial.println(F("=== resetSettings()"));
  mySettings.cookie = cardCookie;
  mySettings.version = 2;
  mySettings.locked = true;
  mySettings.stopWhenCardAway = true;
  writeSettingsToFlash();
}

void migrateSettings(int oldVersion) {
  if (oldVersion == 1) {
    Serial.println(F("=== resetSettings()"));
    Serial.println(F("1 -> 2"));
    mySettings.version = 2;
    mySettings.adminMenuLocked = 0;
    writeSettingsToFlash();
  }
}

void loadSettingsFromFlash() {
  Serial.println(F("=== loadSettingsFromFlash()"));
  int address = sizeof(myFolder->folder) * 100;
  EEPROM.get(address, mySettings);
  if (mySettings.cookie != cardCookie)
    resetSettings();
  migrateSettings(mySettings.version);

  Serial.print(F("Version: "));
  Serial.println(mySettings.version);

  Serial.print(F("Locked: "));
  Serial.println(mySettings.locked);

  Serial.print(F("Sleep Timer: "));
  Serial.println(mySettings.standbyTimer);

  Serial.print(F("Admin Menu locked: "));
  Serial.println(mySettings.adminMenuLocked);

  Serial.print(F("Stop when card away: "));
  Serial.println(mySettings.stopWhenCardAway);
}

class Modifier {
  public:
    virtual void loop() {}
    virtual bool handlePause() {
      return false;
    }
    virtual bool handleNext() {
      return false;
    }
    virtual bool handlePrevious() {
      return false;
    }
    virtual bool handleNextButton() {
      return false;
    }
    virtual bool handlePreviousButton() {
      return false;
    }
    virtual bool handleVolumeUp() {
      return false;
    }
    virtual bool handleVolumeDown() {
      return false;
    }
    virtual bool handleRFID(nfcTagObject *newCard) {
      return false;
    }
    virtual uint8_t getActive() {
      return 0;
    }
    Modifier() {

    }
};

// Leider kann das Modul selbst keine Queue abspielen, daher müssen wir selbst die Queue verwalten
//static uint16_t _lastTrackFinished;
static void nextTrack(uint16_t track) {
  Serial.println(track);

  if (track == _lastTrackFinished) {
    return;
  }
  _lastTrackFinished = track;

  if (knownCard == false)
    // Wenn eine neue Karte angelernt wird soll das Ende eines Tracks nicht
    // verarbeitet werden
    return;

  Serial.println(F("=== nextTrack()"));

  if (currentTrack != numTracksInFolder) {
    currentTrack = currentTrack + 1;
    Serial.print(F("Hörbuch Modus ist aktiv -> nächster Track und "
                   "Fortschritt speichern"));
    Serial.println(currentTrack);
    mp3.playFolderTrack(myFolder->folder, currentTrack);
    // Fortschritt im EEPROM abspeichern
    EEPROM.update(myFolder->folder, currentTrack);
  } else {
    //      mp3.sleep();  // Je nach Modul kommt es nicht mehr zurück aus dem Sleep!
    // Fortschritt zurück setzen
    EEPROM.update(myFolder->folder, 1);
    forgetLastCard=true;
  }
  delay(500);
}

static void previousTrack() {
  Serial.println(F("=== previousTrack()"));
  Serial.println(F("Hörbuch Modus ist aktiv -> vorheriger Track und "
                   "Fortschritt speichern"));
  if (currentTrack != 1) {
    currentTrack = currentTrack - 1;
  }
  mp3.playFolderTrack(myFolder->folder, currentTrack);
  // Fortschritt im EEPROM abspeichern
  EEPROM.update(myFolder->folder, currentTrack);
  delay(1000);
}

// MFRC522
#define RST_PIN 9                 // Configurable, see typical pin layout above
#define SS_PIN 10                 // Configurable, see typical pin layout above
MFRC522 mfrc522(SS_PIN, RST_PIN); // Create MFRC522
MFRC522::MIFARE_Key key;
bool successRead;
byte sector = 1;
byte blockAddr = 4;
byte trailerBlock = 7;
MFRC522::StatusCode status;

#define buttonSelect A0
#define buttonUp A1
#define buttonDown A2
#define buttonAdmin A3
#define buttonSkip A4
#define busyPin 4
#define shutdownPin 7
#define openAnalogPin A6

#define LONG_PRESS 1000

Button selectButton(buttonSelect);
Button upButton(buttonUp);
Button downButton(buttonDown);
Button adminButton(buttonAdmin);
Button skipButton(buttonSkip);
bool ignoreselectButton = false;
bool ignoreUpButton = false;
bool ignoreDownButton = false;
bool ignoreadminButton = false;
bool ignoreskipButton = false;

bool isPlaying() {
  return !digitalRead(busyPin);
}

void waitForTrackToFinish() {
  long currentTime = millis();
#define TIMEOUT 1000
  do {
    mp3.loop();
  } while (!isPlaying() && millis() < currentTime + TIMEOUT); 
  delay(1000);
  do {
    mp3.loop();
  } while (isPlaying());
}

void setup() {
  Serial.begin(115200); // Es gibt ein paar Debug Ausgaben über die serielle Schnittstelle
   
  // Wert für randomSeed() erzeugen durch das mehrfache Sammeln von rauschenden LSBs eines offenen Analogeingangs
  uint32_t ADC_LSB;
  uint32_t ADCSeed;
  for(uint8_t i = 0; i < 128; i++) {
    ADC_LSB = analogRead(openAnalogPin) & 0x1;
    ADCSeed ^= ADC_LSB << (i % 32); 
  }
  randomSeed(ADCSeed); // Zufallsgenerator initialisieren

  // Dieser Hinweis darf nicht entfernt werden
  Serial.println(F("\n _____         _____ _____ _____ _____"));
  Serial.println(F("|_   _|___ ___|  |  |     |   | |     |"));
  Serial.println(F("  | | | . |   |  |  |-   -| | | |  |  |"));
  Serial.println(F("  |_| |___|_|_|_____|_____|_|___|_____|\n"));
  Serial.println(F("TonUINO Version 2.1 + mods by McGreg"));
  Serial.println(F("created by Thorsten Voß and licensed under GNU/GPL."));
  Serial.println(F("Information and contribution at https://tonuino.de.\n"));

  // Busy Pin
  pinMode(busyPin, INPUT);

  // load Settings from EEPROM
  loadSettingsFromFlash();

  // DFPlayer Mini initialisieren
  Serial.println(F("mp3.begin();"));
  mp3.begin();
  // Zwei Sekunden warten bis der DFPlayer Mini initialisiert ist
  Serial.println(F(""));
  Serial.println(F("delay 200ms"));
  delay(200);

  Serial.println(F("mp3.setVolume"));
  mp3.setVolume(25);

  Serial.println(F("mp3.setEq = 0"));
  mp3.setEq(0);

  // NFC Leser initialisieren
  Serial.println(F("SPI.begin"));
  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522
  mfrc522
  .PCD_DumpVersionToSerial(); // Show details of PCD - MFRC522 Card Reader
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  pinMode(buttonSelect, INPUT_PULLUP);
  pinMode(buttonUp, INPUT_PULLUP);
  pinMode(buttonDown, INPUT_PULLUP);
  pinMode(buttonAdmin, INPUT_PULLUP);
  pinMode(buttonSkip, INPUT_PULLUP);
  pinMode(shutdownPin, OUTPUT);
  digitalWrite(shutdownPin, LOW);


  // RESET --- ALLE DREI KNÖPFE BEIM STARTEN GEDRÜCKT HALTEN -> alle EINSTELLUNGEN werden gelöscht
  if (digitalRead(buttonSelect) == LOW && digitalRead(buttonUp) == LOW &&
      digitalRead(buttonDown) == LOW) {
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
    loadSettingsFromFlash();
  }
}

void readButtons() {
  selectButton.read();
  upButton.read();
  downButton.read();
  adminButton.read();
  skipButton.read();
}

void volumeUpButton() {
  Serial.println(F("=== volumeUp()"));
}

void volumeDownButton() {
  Serial.println(F("=== volumeDown()"));
}
void adminButtonAction() {
  Serial.println(F("=== adminButton()"));
}
void skipButtonAction() {
  Serial.println(F("=== skipButton()"));
}
void nextButton() {
  nextTrack(random(65536));
  delay(1000);
}

void previousButton() {
  previousTrack();
  delay(1000);
}

void playFolder() {
  Serial.print(F("== playFolder(): "));
  Serial.println(myFolder->folder);
  knownCard = true;
  _lastTrackFinished = 0;
  numTracksInFolder = mp3.getFolderTrackCount(myFolder->folder);
  Serial.print(numTracksInFolder);
  Serial.print(F(" Dateien in Ordner "));
  Serial.println(myFolder->folder);

// Hörbuch Modus: kompletten Ordner spielen und Fortschritt merken
  Serial.println(F("Hörbuch Modus -> kompletten Ordner spielen und "
                   "Fortschritt merken"));
  currentTrack = EEPROM.read(myFolder->folder);
  if (currentTrack == 0 || currentTrack > numTracksInFolder) {
    currentTrack = 1;
  }
  Serial.print(F("current track according to EEPROM: "));
  Serial.println(currentTrack);
  mp3.playFolderTrack16(myFolder->folder, currentTrack);
  mp3.start(); // Needed because of repeated "Com Error 6" after playFolderTrack/playFolderTrack16 that were not fixable with any of the known fixes in Tonuino FAQ (file naming, sd card change, soldering cleanup)
  Serial.print(F("After: mp3.playFolderTrack(myFolder->folder, currentTrack);"));
}

//Um festzustellen ob eine Karte entfernt wurde, muss der MFRC regelmäßig ausgelesen werden
byte pollCard()
{
  const byte maxRetries = 2;

  if (!hasCard)
  {
    if (mfrc522.PICC_IsNewCardPresent())
    {
      if (mfrc522.PICC_ReadCardSerial())
      {
        Serial.print(F("ReadCardSerial finished: "));
        if (readCard(&myCard))
        {
          bool bSameUID = !memcmp(lastCardUid, mfrc522.uid.uidByte, 4);
          if (bSameUID) {
            Serial.println(F("same card"));
          }
          else {
            Serial.println(F("new card"));
          }
          // store info about current card
          memcpy(lastCardUid, mfrc522.uid.uidByte, 4);
          lastCardWasUL = mfrc522.PICC_GetType(mfrc522.uid.sak) == MFRC522::PICC_TYPE_MIFARE_UL;
    
          retries = maxRetries;
          hasCard = true;
          return bSameUID ? PCS_CARD_IS_BACK : PCS_NEW_CARD;
        }
        else //readCard war nicht erfolgreich
        {
            mfrc522.PICC_HaltA();
            mfrc522.PCD_StopCrypto1();
            Serial.println(F("Karte konnte nicht gelesen werden"));
        }
      }
    }
    return PCS_NO_CHANGE;
  }
  else // hasCard
  {
    // perform a dummy read command just to see whether the card is in range
    byte buffer[18];
    byte size = sizeof(buffer);
    
    if (mfrc522.MIFARE_Read(lastCardWasUL ? 8 : blockAddr, buffer, &size) != MFRC522::STATUS_OK)
    {
      if (retries > 0)
      {
        retries--;
      }
      else
      {
        Serial.println(F("Karte ist weg!"));
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
        hasCard = false;
        return PCS_CARD_GONE;
      }
    }
    else
    {
      retries = maxRetries;
    }
  }
  return PCS_NO_CHANGE;
}

void handleCardReader()
{
  // poll card only every 100ms
  static uint8_t lastCardPoll = 0;
  uint8_t now = millis();
    
  if (static_cast<uint8_t>(now - lastCardPoll) > 100)
  {
    lastCardPoll = now;
    switch (pollCard())
    {
    case PCS_NEW_CARD:
      onNewCard();
      break;
    case PCS_CARD_GONE:
      mp3.pause();
      break;
      
    case PCS_CARD_IS_BACK:
      //nur weiterspielen wenn vorher nicht konfiguriert wurde
      if (!forgetLastCard) 
      {
        uint16_t CurrentTrack = mp3.getCurrentTrack();
        Serial.print(F("resuming play on track: "));
        Serial.println(CurrentTrack);
        mp3.start();
      }
      else 
      {
          onNewCard();
      }
      break;
    }    
  }

}

void loop() {
    mp3.loop();
    // Buttons werden nun über JS_Button gehandelt, dadurch kann jede Taste
    // doppelt belegt werden
    readButtons();

    // admin menu
    if (adminButton.isPressed()) {
      mp3.pause();
      do {
        readButtons();
      } while (adminButton.isPressed());
      readButtons();
      adminMenu();
      return;
    }

    if (selectButton.wasReleased()) {
      if (ignoreselectButton == false)
        if (isPlaying()) {
          mp3.pause();
//          setstandbyTimer();
        }
        else if (knownCard) {
          mp3.start();
//          disablestandbyTimer();
        }
      ignoreselectButton = false;
    } else if (selectButton.pressedFor(LONG_PRESS) &&
               ignoreselectButton == false) {
      if (isPlaying()) {
        mp3.playAdvertisement(currentTrack);
      }
      ignoreselectButton = true;
    }

    if (upButton.wasReleased()) {
      volumeUpButton();
    }
    if (downButton.wasReleased()) {
      volumeDownButton();
    }
    if (adminButton.wasReleased()) {
      adminButtonAction();
    }
    if (skipButton.wasReleased()) {
      skipButtonAction();
    }
    // Ende der Buttons
    handleCardReader();
}

void onNewCard()
{
  Serial.println(F("=== onNewCard()"));
  forgetLastCard=false;
  // make random a little bit more "random"
  randomSeed(millis() + random(1000));
  if (myCard.cookie == cardCookie && myCard.nfcFolderSettings.folder != 0) {
    Serial.println(F("myCard.nfcFolderSettings.folder != 0   =>   playFolder()"));
    Serial.print(F("myCard.nfcFolderSettings.folder: "));
    Serial.println(myCard.nfcFolderSettings.folder);
    playFolder();
  }
  // Neue Karte konfigurieren
  else if (myCard.cookie != cardCookie) {
    knownCard = false;
    mp3.playMp3FolderTrack(300);
    waitForTrackToFinish();
    setupCard();
  }
}

void adminMenu(bool fromCard = false) {
  //Vergesse die vorherige Karte, wenn das Admin Menü betreten wird
  forgetLastCard=true;
  mp3.pause();
  Serial.println(F("=== adminMenu()"));
  knownCard = false;
  int subMenu = voiceMenu(3, 900, 900, false, false, 0, true);
  if (subMenu == 0)
    return;
  if (subMenu == 1) {
    resetCard();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
  else if (subMenu == 2) {
    Serial.println(F("Reset -> EEPROM wird gelöscht"));
    for (int i = 0; i < EEPROM.length(); i++) {
      EEPROM.update(i, 0);
    }
    resetSettings();
    mp3.playMp3FolderTrack(999);
  }
  writeSettingsToFlash();
}

uint8_t voiceMenu(int numberOfOptions, int startMessage, int messageOffset,
                  bool preview = false, int previewFromFolder = 0, int defaultValue = 0, bool exitWithLongPress = false) {
  uint8_t returnValue = defaultValue;
  if (startMessage != 0)
    mp3.playMp3FolderTrack(startMessage);
  Serial.print(F("=== voiceMenu() ("));
  Serial.print(numberOfOptions);
  Serial.println(F(" Options)"));
  do {
    if (Serial.available() > 0) {
      int optionSerial = Serial.parseInt();
      if (optionSerial != 0 && optionSerial <= numberOfOptions)
        return optionSerial;
    }
    readButtons();
    mp3.loop();
    if (selectButton.pressedFor(LONG_PRESS)) {
      mp3.playMp3FolderTrack(802);
      ignoreselectButton = true;
      return defaultValue;
    }
    if (selectButton.wasReleased()) {
      if (returnValue != 0) {
        Serial.print(F("=== "));
        Serial.print(returnValue);
        Serial.println(F(" ==="));
        return returnValue;
      }
      delay(1000);
    }

    if (upButton.pressedFor(LONG_PRESS)) {
      returnValue = min(returnValue + 10, numberOfOptions);
      Serial.println(returnValue);
      //mp3.pause();
      mp3.playMp3FolderTrack(messageOffset + returnValue);
      waitForTrackToFinish();
      ignoreUpButton = true;
    } else if (upButton.wasReleased()) {
      if (!ignoreUpButton) {
        returnValue = min(returnValue + 1, numberOfOptions);
        Serial.println(returnValue);
        //mp3.pause();
        mp3.playMp3FolderTrack(messageOffset + returnValue);
      } else {
        ignoreUpButton = false;
      }
    }

    if (downButton.pressedFor(LONG_PRESS)) {
      returnValue = max(returnValue - 10, 1);
      Serial.println(returnValue);
      //mp3.pause();
      mp3.playMp3FolderTrack(messageOffset + returnValue);
      waitForTrackToFinish();
      ignoreDownButton = true;
    } else if (downButton.wasReleased()) {
      if (!ignoreDownButton) {
        returnValue = max(returnValue - 1, 1);
        Serial.println(returnValue);
        //mp3.pause();
        mp3.playMp3FolderTrack(messageOffset + returnValue);
      } else {
        ignoreDownButton = false;
      }
    }
  } while (true);
}

void resetCard() {
  mp3.playMp3FolderTrack(800);
  do {
    selectButton.read();
    upButton.read();
    downButton.read();

    if (upButton.wasReleased() || downButton.wasReleased()) {
      Serial.println(F("Abgebrochen!"));
      mp3.playMp3FolderTrack(802);
      return;
    }
  } while (!mfrc522.PICC_IsNewCardPresent());

  if (!mfrc522.PICC_ReadCardSerial())
    return;

  Serial.println(F("Karte wird neu konfiguriert!"));
  setupCard();
}

bool setupFolder(folderSettings * theFolder) {
  // Ordner abfragen
  theFolder->folder = voiceMenu(99, 301, 0, true, 0, 0, true);
  if (theFolder->folder == 0) return false;

  //  // Hörbuchmodus -> Fortschritt im EEPROM auf 1 setzen
  //  EEPROM.update(theFolder->folder, 1);

  // Admin Funktionen
//  if (theFolder->mode == 6) {
//    theFolder->folder = 0;
//    theFolder->mode = 255;
//  }
  return true;
}

void setupCard() {
  mp3.pause();
  Serial.println(F("=== setupCard()"));
  nfcTagObject newCard;
  if (setupFolder(&newCard.nfcFolderSettings) == true)
  {
    // Karte ist konfiguriert -> speichern
    mp3.pause();
    do {
    } while (isPlaying());
    writeCard(newCard);
    forgetLastCard=true;
  }
  delay(1000);
}
bool readCard(nfcTagObject * nfcTag) {
  nfcTagObject tempCard;
  // Show some details of the PICC (that is: the tag/card)
  Serial.print(F("Card UID:"));
  dump_byte_array(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.println();
  Serial.print(F("PICC type: "));
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.println(mfrc522.PICC_GetTypeName(piccType));

  byte buffer[18];
  byte size = sizeof(buffer);

  // Authenticate using key A
  if ((piccType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.println(F("Authenticating Classic using key A..."));
    status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &key, &(mfrc522.uid));
  }
  else if (piccType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte pACK[] = {0, 0}; //16 bit PassWord ACK returned by the tempCard

    // Authenticate using key A
    Serial.println(F("Authenticating MIFARE UL..."));
    status = mfrc522.PCD_NTAG216_AUTH(key.keyByte, pACK);
  }

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    return false;
  }

  // Show the whole sector as it currently is
   Serial.println(F("Current data in sector:"));
   mfrc522.PICC_DumpMifareClassicSectorToSerial(&(mfrc522.uid), &key, sector);
   Serial.println();

  // Read data from the block
  if ((piccType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (piccType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.print(F("Reading data from block "));
    Serial.print(blockAddr);
    Serial.println(F(" ..."));
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(blockAddr, buffer, &size);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }
  }
  else if (piccType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte buffer2[18];
    byte size2 = sizeof(buffer2);

    status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(8, buffer2, &size2);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_1() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }
    memcpy(buffer, buffer2, 4);

    status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(9, buffer2, &size2);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_2() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }
    memcpy(buffer + 4, buffer2, 4);

    status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(10, buffer2, &size2);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_3() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }
    memcpy(buffer + 8, buffer2, 4);

    status = (MFRC522::StatusCode)mfrc522.MIFARE_Read(11, buffer2, &size2);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("MIFARE_Read_4() failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return false;
    }
    memcpy(buffer + 12, buffer2, 4);
  }

  Serial.println(F("Data on Card: "));
  dump_byte_array(buffer, 16);
  Serial.println();

  uint32_t tempCookie;
  tempCookie = (uint32_t)buffer[0] << 24;
  tempCookie += (uint32_t)buffer[1] << 16;
  tempCookie += (uint32_t)buffer[2] << 8;
  tempCookie += (uint32_t)buffer[3];

  tempCard.cookie = tempCookie;
  tempCard.version = buffer[4];
  tempCard.nfcFolderSettings.folder = buffer[5];

  if (tempCard.cookie == cardCookie) {
    Serial.println(F("tempCard.cookie == cardCookie"));
    if (tempCard.nfcFolderSettings.folder == 0) {
//      switch (tempCard.nfcFolderSettings.mode ) {
//        case 0:
//        case 255:
//          mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1(); adminMenu(true);  break;
//      }
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      delay(2000);
      return false;
    }
    else {
      memcpy(nfcTag, &tempCard, sizeof(nfcTagObject));
      Serial.print(F("nfcTag->nfcFolderSettings.folder: "));
      Serial.println( nfcTag->nfcFolderSettings.folder);
      myFolder = &nfcTag->nfcFolderSettings;
      Serial.print(F("myFolder->folder: "));
      Serial.println( myFolder->folder);
    }
    return true;
  }
  else {
    memcpy(nfcTag, &tempCard, sizeof(nfcTagObject));
    return true;
  }
}

void writeCard(nfcTagObject nfcTag) {
  MFRC522::PICC_Type mifareType;
  byte buffer[16] = {0x13, 0x37, 0xb3, 0x47, // 0x1337 0xb347 magic cookie to
                     // identify our nfc tags
                     0x02,                   // version 1
                     nfcTag.nfcFolderSettings.folder,          // the folder picked by the user
//                     nfcTag.nfcFolderSettings.mode,    // the playback mode picked by the user
//                     nfcTag.nfcFolderSettings.special, // track or function for admin cards
//                     nfcTag.nfcFolderSettings.special2,
                     0x00, 0x00, 0x00, // replaced the above 3 values by 0x00
                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                    };

  byte size = sizeof(buffer);

  mifareType = mfrc522.PICC_GetType(mfrc522.uid.sak);

  // Authenticate using key B
  //authentificate with the card and set card specific parameters
  if ((mifareType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    Serial.println(F("Authenticating again using key A..."));
    status = mfrc522.PCD_Authenticate(
               MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &key, &(mfrc522.uid));
  }
  else if (mifareType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte pACK[] = {0, 0}; //16 bit PassWord ACK returned by the NFCtag

    // Authenticate using key A
    Serial.println(F("Authenticating UL..."));
    status = mfrc522.PCD_NTAG216_AUTH(key.keyByte, pACK);
  }

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    mp3.playMp3FolderTrack(401);
    return;
  }

  // Write data to the block
  Serial.print(F("Writing data into block "));
  Serial.print(blockAddr);
  Serial.println(F(" ..."));
  dump_byte_array(buffer, 16);
  Serial.println();

  if ((mifareType == MFRC522::PICC_TYPE_MIFARE_MINI ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_1K ) ||
      (mifareType == MFRC522::PICC_TYPE_MIFARE_4K ) )
  {
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(blockAddr, buffer, 16);
  }
  else if (mifareType == MFRC522::PICC_TYPE_MIFARE_UL )
  {
    byte buffer2[16];
    byte size2 = sizeof(buffer2);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer, 4);
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(8, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 4, 4);
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(9, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 8, 4);
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(10, buffer2, 16);

    memset(buffer2, 0, size2);
    memcpy(buffer2, buffer + 12, 4);
    status = (MFRC522::StatusCode)mfrc522.MIFARE_Write(11, buffer2, 16);
  }

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("MIFARE_Write() failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    mp3.playMp3FolderTrack(401);
  }
  else
    mp3.playMp3FolderTrack(400);
  Serial.println();
  delay(2000);
}

/**
  Helper routine to dump a byte array as hex values to Serial.
*/
void dump_byte_array(byte * buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
}
