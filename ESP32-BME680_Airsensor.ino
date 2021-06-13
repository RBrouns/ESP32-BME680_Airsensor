// How to Flash
// Hold BOOT button on ESP32 while IDE tries to upload.
// Appears at COM20
//	
// ESP32: https://www.studiopieters.nl/esp32-pinout/
// ESP32: https://lastminuteengineers.com/esp32-sleep-modes-power-consumption/
//
// Configuring environment to allow BSEC library to compile:
// Help on integration BSEC for BME680: https://community.bosch-sensortec.com/t5/MEMS-sensors-forum/Using-the-BSEC-Library-with-a-ESP32/td-p/18090
// Additional link: https://www.bluedot.space/tutorials/air-quality-measurement-with-the-bme680/ 
// The ESP32-specific platform.txt in (C:\Users\username\AppData\Local\Arduino15\packages\esp32\hardware\) had to be modified for compiling to work.

#include <esp_wifi.h>
#include "bsec.h"
#include <sys/time.h>					//Needed to obtain millis references, even after Deep Sleep
#include <BlynkSimpleEsp32.h>
#include <string>

#define WIFI_SLEEP_TIME_S		120
#define BTN_ONBOARD_BOOT		0			//GPIO 0, Boot button. LOW when pressed (pulled high)
#define LED_ONBOARD 			1			//GPIO 1, TX, LED works only when serial is disabled
#define LED_EXT_RED				25			//GPIO25
#define LED_EXT_GREEN			26			//GPIO26
#define BME680_VCC				32			//GPIO32

#define LED_ON					LOW
#define LED_OFF					HIGH

#define NVM_KEY_SSID			"ssid"
#define NVM_KEY_PASS			"pass"
#define NVM_KEY_BLYNK			"blynkAuth"

//Timers
#include <Ticker.h>
Ticker ledBlinkTimer;
Ticker wifiWakeTimer;

//Function definition
void setupInterfaces();
bool connectBme680();
bool checkBmeStatus();
void preconfigureWifi();
bool connectWifi();

//BME and BSEC library
Bsec bme;
const uint8_t bsec_config_iaq[] = {
	#include "config/generic_33v_3s_4d/bsec_iaq.txt"
};
uint16_t calibrationIncr = 0;
float compensatedTemp;
float humidity;
float staticIaq;
int8_t iaqAccuracy;

//Variables stored in RTC Recovery memory (retained during Deep sleep)
RTC_DATA_ATTR uint64_t deepSleepStartTime = 0;
RTC_DATA_ATTR uint8_t savedSensorState[BSEC_MAX_STATE_BLOB_SIZE] = {0};

//Wifi and Blynk
#include <Preferences.h>
Preferences preferences;
String wifiSSID;					//TBD: Check if pref library supports char-array instead
String wifiPass;
const String wifiName = "ESP32-BME680Airsensor";
String blynkAuth;

//Blynk
WidgetBridge blynkBridge(V10);		//Send debug info to central device

// Entry point for the example
void setup(void)
{	
	pinMode(BTN_ONBOARD_BOOT, INPUT);
	pinMode(LED_EXT_RED, OUTPUT);
	pinMode(LED_EXT_GREEN, OUTPUT);
	digitalWrite(LED_EXT_RED, LED_OFF);
	
	ledBlinkTimer.attach(1, ledToggle);
	
	setupInterfaces();
	configureEsp();
	
	connectBme680();
	configureBme680();
  
	preconfigureWifi();
	connectWifi();
}

void loop(void)
{
	if(!bme.run(GetTimestamp())){
		return;
	}
	
	//New sensor data available
	blynkBridge.virtualWrite(V13, "Keep-alive check-in " + wifiName);	//Inform central device on its V13 port
	bme680TakeMeasurement();
	bool isCalibrated = bme680TakeIaq();
	
	if(isWifiConnected()){
		Blynk.virtualWrite(V0, compensatedTemp);
		Blynk.virtualWrite(V10, String(compensatedTemp,1) + "°C");
		Blynk.virtualWrite(V1, humidity);
		Blynk.virtualWrite(V3, (iaqAccuracy == 0? "--" : String(staticIaq,1)));
		Blynk.setProperty(V3, "color", (staticIaq < 75? "#69e089":"#ed4747"));
		Blynk.run();
		wifiSleep();
		wifiWakeTimer.once(WIFI_SLEEP_TIME_S, wifiWake);
	}
	
	if(isCalibrated){
		stopLedToggle();
		deepSleepStartTime = GetTimestamp();
		bme.getState(savedSensorState);		//Store latest state in RTC
		checkBmeStatus();	
	}
}

void ledToggle(){
	digitalWrite(LED_EXT_GREEN, !digitalRead(LED_EXT_GREEN));
}

void stopLedToggle(){
	ledBlinkTimer.detach();
	digitalWrite(LED_EXT_GREEN, LED_ON);
}

// ---------------------
// Interfaces such as Serial
// ---------------------

void setupInterfaces(){
	Serial.begin(115200);
	while(!Serial){
		delay(10);
	}
}
	
//----------------------
// WiFi
// ---------------------

void preconfigureWifi(){
	preferences.begin("wificredentials", false);
	wifiSSID = preferences.getString(NVM_KEY_SSID,"");
	wifiPass = preferences.getString(NVM_KEY_PASS,"");
	blynkAuth = preferences.getString(NVM_KEY_BLYNK,"");
	preferences.end();
	WiFi.hostname(wifiName);
	//WiFi.setAutoConnect(true);			//Doesn't work (yet) on ESP32
	//WiFi.setAutoReconnect(true);			//Doesn't work (yet) on ESP32
}

bool connectWifi(){
	//Check if Wifi wasn't already in flash, otherwise it will connect automatically (NOTE: Auto-connect not working yet for ESP32)
	Serial.println("Connecting to: " + String(wifiSSID.c_str()) + ", pass:" + String(wifiPass.c_str()));
	int attempts = 0;
	if (WiFi.SSID() != wifiSSID || !isWifiConnected()) {
		while(!isWifiConnected() && attempts++ < 10){
			Serial.println("Connecting to Wifi, attempt: " + String(attempts));
			WiFi.begin(wifiSSID.c_str(),wifiPass.c_str());			//TBD: Is this library function a blocking call?
			delay(5000);
		}
		if(!isWifiConnected()){
			onError(2);
		}
	}
	
	Blynk.config(blynkAuth.c_str());
	attempts = 0;
	while(!Blynk.connected() && attempts++ < 5){
		Serial.println("Connecting to Blynk, attempt: " + String(attempts));
		Blynk.connect();			//TBD: Is blocking call?
		delay(2000);
	}
	if(!Blynk.connected()){
		onError(3);
	}
	
	return true;
}

void wifiSleep(){
	esp_wifi_disconnect();
	esp_wifi_stop();
	//esp_wifi_deinit();
}

void wifiWake(){
	esp_wifi_start();
	connectWifi();
}

//----------------------
// BME680
// ---------------------

bool connectBme680(){
	pinMode(BME680_VCC, OUTPUT);
	digitalWrite(BME680_VCC, HIGH);
	delay(500);
	Wire.begin();
	bme.begin(BME680_I2C_ADDR_SECONDARY, Wire);
	Serial.println("BSEC library version " + String(bme.version.major) + "." + String(bme.version.minor) + "." + String(bme.version.major_bugfix) + "." + String(bme.version.minor_bugfix));
	checkBmeStatus();
}

bool configureBme680(){
	bme.setConfig(bsec_config_iaq);
	checkBmeStatus();
	if(deepSleepStartTime != 0){
		//Serial.println("Info: Sensor state saved at " + String(deepSleepStartTime) + " was retrieved from RTC memory");
		bme.setState(savedSensorState);
		checkBmeStatus();
	}
	
	bsec_virtual_sensor_t sensorList[10] = {
		BSEC_OUTPUT_RAW_TEMPERATURE,
		BSEC_OUTPUT_RAW_PRESSURE,
		BSEC_OUTPUT_RAW_HUMIDITY,
		BSEC_OUTPUT_RAW_GAS,
		BSEC_OUTPUT_IAQ,
		BSEC_OUTPUT_STATIC_IAQ,			
		BSEC_OUTPUT_CO2_EQUIVALENT,
		BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
		BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
		BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
	};

	bme.updateSubscription(sensorList, 10, BSEC_SAMPLE_RATE_LP);			//3 second intervals
	checkBmeStatus();
}

bool checkBmeStatus()
{
	if(bme.status < BSEC_OK) {
		Serial.println("ERR: BSEC Library error: " + String(bme.status));
		onError(1);	//Stop operation and enter Error mode
	}else if(bme.status > BSEC_OK){
		Serial.println("WARN: BSEC warning: " + String(bme.status));
	}

	if(bme.bme680Status < BME680_OK) {
		Serial.println("ERR: BME680 Device Error : " + String(bme.bme680Status));
		onError(1);	//Stop operation and enter Error mode
	}else if(bme.bme680Status > BME680_OK) {
		Serial.println("WARN: BME680 Device warning: " + String(bme.bme680Status));
	}
}

void bme680TakeMeasurement(){
	compensatedTemp = bme.temperature;
	humidity = bme.humidity;
	Serial.println("Data: Temp:" + String(compensatedTemp,1) + ", humidity:" + String(humidity,1));
}

bool bme680TakeIaq(){
	iaqAccuracy = bme.iaqAccuracy;
	staticIaq = bme.staticIaq;
	if(iaqAccuracy != 3){
		Serial.println("Warn: IAQ calibrating, sIAQ value: " + String(staticIaq,1) + ", run: " + String(calibrationIncr++) + ", at time: " + getTimeString() + ", now at BSEC accuracy: " + String(iaqAccuracy) + "/3.");
	}
	return iaqAccuracy == 3;
}

// -------------------------
// Diagnostics
// -------------------------

void configureEsp(){
	uint32_t HwCrystalFreq = getXtalFrequencyMhz();
	uint32_t HwCpuFreq = getCpuFrequencyMhz();
	Serial.println("Diag: Crystal freq: " + String(HwCrystalFreq) + ", CPU clocked at: " + String(HwCpuFreq));
	
	if(HwCpuFreq != 40){
		/*
		if(!setCpuFrequencyMhz(80)){
			Serial.println("ERR: Attempted to set CPU to 40Mhz but was rejected");
		}*/
	}
}

void enableFlashing(){
	while(1){
		digitalWrite(LED_EXT_GREEN, !digitalRead(LED_EXT_GREEN));
	}
}

void onError(int errorCode){
	digitalWrite(LED_EXT_GREEN, LED_OFF);
	for(int i=0;i<errorCode+1;i++){
		digitalWrite(LED_EXT_RED, !digitalRead(LED_EXT_RED));
	}
	Serial.println("Resetting in 3s, error code: " + String(errorCode)+"\r\n");
	digitalWrite(BME680_VCC, LOW);
	delay(3000);
	ESP.restart();
}

// -----------------
// Helpers
// -----------------

bool isWifiConnected(){
	return WiFi.status() == WL_CONNECTED;
}

int64_t GetTimestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

String getTimeString(){
	char buf[40];
	sprintf(buf,"%lld", GetTimestamp());
	return buf;
}