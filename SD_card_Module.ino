#include <SPI.h>
#include <FS.h>
#include <SD.h>

const int chipSelect = 5;

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing.");
  if (!SD.begin(chipSelect)) {
    Serial.println("Card Mount Failed.");
    return;
  }
  Serial.println("SD card ready.");
  randomSeed(analogRead(34));
}

void loop() {
  File writeFile = SD.open("/random_data.txt", FILE_APPEND);
  for (int i = 0; i < 10; i++) {
    int rndm = random(1, 1001);
    Serial.print("Generated a random number ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(rndm);

    
    if (writeFile) {
      writeFile.println(rndm);
      
      Serial.println("Data is saved to SD card");
    } else {
      Serial.println("Error opening file for writing.");
    }
    delay(2000);
    
  }
  writeFile.close();
  File readFile = SD.open("/random_data.txt", FILE_READ);
  if (readFile) {
    Serial.println("Current history in the SD card: ");
    while (readFile.available()) {
      Serial.write(readFile.read());
    }
    readFile.close();
  } else {
    Serial.println("Error opening a file for reading.");
  }
  delay(5000);
  SD.remove("/random_data.txt");
}
