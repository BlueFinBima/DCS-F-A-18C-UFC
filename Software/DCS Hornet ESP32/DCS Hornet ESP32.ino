/*
*  This sketch is the first iteration of the WiFi DCS F/A-18C cockpit controller based on the ESP32
*
*  requires fixed ESP32 I2C files from https://github.com/stickbreaker/arduino-esp32/releases 
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
*
*  On first running, a wireless hotspot (SSID "BlueFinBima UFC" PW "bluefinbima") and you connect to the
*  WiFi configuration menu by pointing your browser to http://192.168.4.1/config if connecting to the hotspot
*  does not take you to the config screen automatically.
* 
*/

#include "FA18CkeyMappings.h"
#include "AV8BkeyMappings.h"          // Key mappings for the AV-8B
#include "FA18CufcDisplay.h"		  // F/A-18C UFC Display function
#include <arduino.h>
#include <U8g2lib.h>                  // Graphics for the OLED graphic display https://www.arduino.cc/reference/en/libraries/u8g2/
#include <WiFi.h>					  // ESP32 wireless code
#include <AsyncUDP.h>				  // Use async UDP library on ESP32
#include "driver/gpio.h"    
#include <Wire.h>                     // This is needed for i2c communications
#include <OLED_I2C.h>                 // Library for the OLED display without serial board 4-bit
#include "RotaryEncoderAdvanced.h"    // enjoyneering Rotary Encoder functions https://github.com/enjoyneering/RotaryEncoder
#include "RotaryEncoderAdvanced.cpp"  //for some reason linker can't find the *.cpp :(
#if __has_include("WiFiInfo.h")
#include "WiFiInfo.h"                 // Personal Wifi information
#endif
#if __has_include("Personal.h")
#include "Personal.h"                //  Used to denote personal variations from the official schematics in the project
#endif
#include <DNSServer.h>				// https://github.com/prampec/IotWebConf
#include <WebServer.h>				// https://github.com/prampec/IotWebConf
#include <IotWebConf.h>				// https://github.com/prampec/IotWebConf
#include <time.h>	

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 3600;
struct tm timeinfo;
bool ufc_Idle = true;
long lastUFCActivityTime = 0;

#define CONFIG_VERSION "UFC001"
const char apName[] = "BlueFinBima UFC";
const char wifiInitialApPassword[] = "bluefinbima";
// -- Callback method declarations.
void iotWebConfConfigSaved();
boolean htmlFormValidator();

DNSServer dnsServer;
WebServer server(80);

#define STRING_LEN 16
IPAddress ipAddress;
IPAddress gateway;
IPAddress netmask;
char ipAddressValue[STRING_LEN];
char gatewayValue[STRING_LEN];
char netmaskValue[STRING_LEN];

IotWebConf iotWebConf(apName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameter ipAddressParam = IotWebConfParameter("IP address", "ipAddress", ipAddressValue, STRING_LEN, "text", NULL, "10.1.1.48");
IotWebConfParameter gatewayParam = IotWebConfParameter("Gateway", "gateway", gatewayValue, STRING_LEN, "text", NULL, "10.1.1.1");
IotWebConfParameter netmaskParam = IotWebConfParameter("Subnet mask", "netmask", netmaskValue, STRING_LEN, "text", NULL, "255.255.255.0");


// WiFi network name and password:
#ifdef WIFISSID
const char * networkName = WIFISSID;
const char * networkPswd = WIFIPW;
#elif
const char * networkName = "YourSSID";
const char * networkPswd = "Your WiFi Password";
#endif
//IP address to send UDP data to:
// either use the ip address of the server or 
// a network broadcast address
const char * udpAddress = "0.0.0.0";
const uint16_t udpPort = 9089;
IPAddress ip;
IPAddress ipRemote;
uint16_t portRemote = 0;
#define MAXPACKETSIZE 600

AsyncUDP udp;

// the Export send code does a >576 test before sending so the max data is going to be 576 + length of the longest command
char packetBuffer[MAXPACKETSIZE];		// network buffer to hold incoming packet
volatile int packetLen = 0;				// length of the data in the packet buffer 
char cmdBuffer[MAXPACKETSIZE];			// this is a buffer to hold a single command (there can be long text command values
char cmdValStrBuffer[32];				// Used for the conversion of the comand value
char replyBuffer[32];					// a string to send back
bool connected = false;					// Indicate if we are currently connected
										// Constructor for the UDP library class
unsigned long time1 = 0;                // Used to calculate loop delay
unsigned long time2 = 0;                // Used to calculate loop delay
										//
										// These are the response which checking keys can return
#define SWITCHNONE 0 
#define SWITCHWAIT 1
#define SWITCHSET 2
#define SWITCHALREADYDONE 3

//
//
//  i2c Constants
//
//  This section is the master list of all of the 12c devices in use in the project
//
const uint8_t i2c_addr_other = 0x78;     // dummy i2c address for the simulated HT16K33 used for oddly connected switches
const uint8_t i2c_addr_ufc_expander = 0x20;  // i2c expander on the F/A-18C UFC which controls the ODU's and main display
const uint8_t i2c_addr_ufc_mux = 0x70;       // i2c multiplexer on the F/A-18C UFC
const uint8_t i2c_addr_ufc = 0x71;       // i2c address of the HT16K33 used for the F/A-18C up front controller keys, illuminations, Cues & Comm Channel Displays (This has no indicators)
const uint8_t i2c_addr_landingGear = 0x72;     // i2c address of the HT16K33 used for the RHS switches (This is a general switch board)
										//
										//
										//
										// List here the highest and lowest i2c address for the HT16K33 chips.  These are managed to 
										// ensure that only the correct number of buffers are allocated.
#define LOWEST_I2C_ADDR 0x71            //this is the address of the lowest i2c HT16K33
#define HIGHEST_I2C_ADDR 0x74           //this is the address of the highest i2c HT16K33 +2
										//

bool readkeys = false;

//  HT16K33 Constants
//
// Various HT16K33 constants etc 
#define HT16K33_BLINK_CMD 0x80
#define HT16K33_BLINK_DISPLAYON 0x01
#define HT16K33_BLINK_OFF 0
#define HT16K33_BLINK_2HZ  1
#define HT16K33_BLINK_1HZ  2
#define HT16K33_BLINK_HALFHZ  3
#define HT16K33_CMD_BRIGHTNESS 0xE0
#define HT16K33_KEY_RAM_ADDR 0x40        //this is the six bytes which hold the keys which have been pressed
#define HT16K33_KEY_INTERUPT_ADDR 0x60   
#define HT16K33_KEY_ROWINT_ADDR 0xA0   
//
#define HT16K33_DEFAULT_BRIGHTNESS 0x15

//
uint16_t displaybuffer[HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR][8];
bool HT16K33Push[HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR]; // Used to force a write to a particular HT16K33 chip
int16_t  kk[3];            // this is the integer array to contain a keycode and value pair
int16_t  keystack[3][64];  // this is the FIFO stack of key presses, key values and devices
uint8_t  keystackNext;     // this is the next keypress to be processed
uint8_t  keystackFree;     // this is the first free slot in the keystack 
uint8_t keys[HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR][6];
uint8_t lastkeys[HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR][6];
unsigned long switchLastIntTime[HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR + 1];  // this is the time when the last interrupt was seen from the device
//void connectToWiFi(const char *, const char *);

uint8_t ufcType = 0;		// Indicator to support different aircraft types.  Default is F/A-18C

#define NUMBEROFINDICATORS 10
const uint8_t generalIndicators[3][NUMBEROFINDICATORS] = {  // these are the LED positions for various indicators * * * Do Not Change Order without addjusting cmdOffset usage
	{
	12,12,                // Flaps Lights 
	14,14,14,             // Gear Ready Lights x3 (L,N,R) 
	13,13,13,      		  // Gear amber lights x3 (launch Bar, SpeedBrake , hook)     
	15,					  // Landing Gear Lollipop Light
	11					  // Landing Gear Lollipop Light
	},
	{
	6,4,
	4,6,5,
	4,6,5,
	4,
	6
	},
	{
	i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,
	i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,
	i2c_addr_landingGear,
	i2c_addr_landingGear
	}
};
const uint8_t generalIndicatorsAV8B[3][NUMBEROFINDICATORS] = {  // these are the LED positions for various indicators * * * Do Not Change Order without addjusting cmdOffset usage
	{
    13,14,13,14, 
	13,14,12,12,  
	15,					  // Landing Gear Lollipop Light
	11					  // Landing Gear Lollipop Light
	},
	{
	6,6,4,4,
	5,5,6,4,
	4,
	6
	},
	{
	i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,
	i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,i2c_addr_landingGear,
	i2c_addr_landingGear,
	i2c_addr_landingGear
	}
};


#define MAXENCODERS 16
float EncoderValues[MAXENCODERS];            // This array holds the values of the encoders which get used when the encoders are reporting as if they are keys
									//
char displayBuffer[21];
char i2cBuffer[128];

#define LED_ON 1
#define LED_OFF 0
//
// General String variables
String command;
String value;
//
int i = 0;
int j = 0;
//
bool commandsReady = false;

//
// New devices for the Hornet

FA18CufcDisplay UFCDisplay(i2c_addr_ufc_expander);

// UFC status OLED
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C oledUFCstatus(U8G2_R2);  //  Rotate display by 180 deg
char oledUFCstatusBuffer[32];

//These are the character displays for the 16 segment channel displays on the UFC
//
//			   LED segment values
//           
//          , 0x0001,       , 0x0002,
//    0x0080, 0x0100, 0x0200, 0x0400, 0x0004
//		    , 0x8000,       , 0x0800,
//    0x0040, 0x4000, 0x2000, 0x1000, 0x0008
//          , 0x0020,       , 0x0010,
//
const uint16_t UFCchannelNumbers[29] = {
	0x0000, 0x000C, 0x2816, 0x081E, 
	0x0A0C, 0x0A1A, 0x2A18, 0x000E, 
	0x2A1E, 0x0A0E, 0x22DE, 0x00CC, 
	0x28D6, 0x08DE, 0x0ACC, 0x0ADA,
	0x2AD8, 0x00CE, 0x2ADE, 0x0ACE, 
	0xA27F, 0x08FB, 0x05CC, 0x00F3, 
	0x88BB, 0x0503, 0x5030, 0xAA00,
	0x8800								// final 2 characters are "+" and "-"
};

// These are the addresses of the sets of two UFCcueing LEDs on the UFC
const uint8_t UFCcueing[5] = { 1,3,5,7,9};

// Onboard Encoders
volatile byte encInt = 0;  // set in the ISR with a bit for a change to each of the encoders.

#define D1 22  
#define D2 21  
#define D3 18  
#define D4 19  
#define D5 16  
#define D6 17  
#define D7 5  
#define D8 4  
#define D9 23  
#define D10 0

void IRAM_ATTR UFCBrightnessISR();
void IRAM_ATTR UFCVolume1ISR();
void IRAM_ATTR UFCVolume2ISR();
void IRAM_ATTR UFCChannel1ISR();
void IRAM_ATTR UFCChannel2ISR();

#ifndef PERSONAL
RotaryEncoderAdvanced <float> UFCChannel1(12, 13, 0xff, 0.05, 0, 1);
RotaryEncoderAdvanced <float> UFCChannel2(27, 26, 0xff, 0.05, 0, 1);
RotaryEncoderAdvanced <float> UFCBrightness(17, 16, 0xff, 0.1, 0, 1);
RotaryEncoderAdvanced <float> UFCVolume1(5, 4, 0xff, 0.1, 0, 1);
RotaryEncoderAdvanced <float> UFCVolume2(19, 18, 0xff, 0.1, 0, 1);
#else
// Change of wiring on 001
RotaryEncoderAdvanced <float> UFCChannel1(25, 15, 0xff, 0.05, 0, 1);
RotaryEncoderAdvanced <float> UFCChannel2(27, 26, 0xff, 0.05, 0, 1);
RotaryEncoderAdvanced <float> UFCBrightness(17, 16, 0xff, 0.1, 0, 1);
RotaryEncoderAdvanced <float> UFCVolume1(5, 4, 0xff, 0.1, 0, 1);
RotaryEncoderAdvanced <float> UFCVolume2(19, 18, 0xff, 0.1, 0, 1);
#endif // PERSONAL
float UFCBrightnessVal;
float UFCVolume1Val;
float UFCVolume2Val;
float UFCChannel1Val;
float UFCChannel2Val;
unsigned long encChannel1Time;
unsigned long encChannel2Time;

// use first channel of 16 channels (started from zero)
#define DLG2416_CHANNEL_0     0
#define ENCODER_LED_CHANNEL_1 1
// use 13 bit precission for LEDC timer
#define DLG2416_TIMER_13_BIT  13
// use 5000 Hz as a LEDC base frequency
#define DLG2416_BASE_FREQ     5000
#define DLG2416_BLANK_PIN     14
//

void setup() {
	// Set the reset pin of the MUX to low
	pinMode(23, OUTPUT);
	digitalWrite(23, HIGH);

	Serial.begin(115200);  // Initilize hardware serial:
	Serial.println("\n");
	//Wire.begin(D2, D1, 25000U);       //  sda = gpio21/D2   scl=gpio22/D1
	Wire.begin(D2, D1);                 //  sda = gpio21/D2   scl=gpio22/D1
	Wire.endTransmission();             // try to send a stop bit to see if this can wake up a sleeping 12c bus
	//Wire.setClock(50000);
	for (uint8_t i = 0; i<HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR; i++) HT16K33Push[i] = false;   // This flag is used to indicate that the LED buffer needs to be written to the HT16K33
	for (uint8_t i = LOWEST_I2C_ADDR; i< HIGHEST_I2C_ADDR; i++) initHT16K33(i);              // set up the HT16K33 i2c chips
																						   //initialise the key buffers  
	UFCDisplay.clear();
	oledUFCstatus.begin();                            // Initialise the status OLED on the UFC
	oledUFCMsg(10, 7, "F/A-18C");
	oledUFCstatus.drawStr(10, 15, "UFC V0.5");
	oledUFCstatus.sendBuffer();

	testI2CScanner();

	muxSelect(0); // Allow 0x72 to be referenced on channel 0
	for (uint8_t i = 0; i<HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR; i++) {
		initHT16K33(LOWEST_I2C_ADDR + i);
		for (uint8_t j = 0; j<6; j++) {
			keys[i][j] = (uint8_t)0;
		}
	}
	muxSelect(8); // Turn off all external channels

	/* Debug
	for(uint8_t ii = LOWEST_I2C_ADDR;ii<HIGHEST_I2C_ADDR+1;ii++){  //this obtains a print of all the key ram
	getDefaultKeys(ii);
	}
	*/

	//  Read the switch data to see if user is attempting to force a network reconfiguration
	//  We check if I/P or ODU1 buttons are depressed to indicate reconfiguration is needed.
	getSwitchData(i2c_addr_ufc);
	bool reconfig = false;
	int accessPointTimeOut = 1000;
	if (keys[i2c_addr_ufc - LOWEST_I2C_ADDR][2] == 0x01 || keys[i2c_addr_ufc - LOWEST_I2C_ADDR][3] == 0x01) {
		reconfig = true;
		accessPointTimeOut = 120000;  // Set access point timeout to 2 mins.
		Serial.println("Reconfiguration has been requested");
	}
	// Setup timer and attach timer to a GPIO pin to allow PWM for the DLG2416 displays
	ledcSetup(DLG2416_CHANNEL_0, DLG2416_BASE_FREQ, DLG2416_TIMER_13_BIT);
	ledcAttachPin(DLG2416_BLANK_PIN, DLG2416_CHANNEL_0);
	DLG2416_Brightness(DLG2416_CHANNEL_0, 255);

	testAll();
	//
	// Get a connection, or configure the AP and then get a connection
	//
	if(reconfig) configUFCMessage();

	iotWebConf.init();
	iotWebConf.setApTimeoutMs(accessPointTimeOut);  // for a device which is already configured, this will wait for 1s for the AP to be connected to
	iotWebConf.addParameter(&ipAddressParam);
	iotWebConf.addParameter(&gatewayParam);
	iotWebConf.addParameter(&netmaskParam);
	iotWebConf.setConfigSavedCallback(&iotWebConfConfigSaved);
	iotWebConf.setFormValidator(&htmlFormValidator);
	iotWebConf.setApConnectionHandler(&connectAp);
	iotWebConf.setWifiConnectionHandler(&connectWifi);

	// -- Set up required URL handlers on the web server.
	server.on("/", handleRoot);
	server.on("/config", [] { iotWebConf.handleConfig(); });
	server.onNotFound([]() { iotWebConf.handleNotFound(); });


	//Connect to the WiFi network
	//connectToWiFi(networkName, networkPswd);

	while (WiFi.status() != WL_CONNECTED) {
		delay(1000);
		iotWebConf.doLoop();
	}
	ip = WiFi.localIP();
	Serial.println("WiFi connected");
	Serial.print("IP address: ");
	Serial.println(ip);

	configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // get time info before starting the UDP Listener

	//WiFi.onEvent(WiFiEvent);
	startUDPListener();

	oledUFCMsg(0,31,"Connected");
	sprintf(oledUFCstatusBuffer, "%d.%d.%d.%d:%d", ip[0], ip[1], ip[2], ip[3], udpPort);
	oledUFCMsg(0, 31, oledUFCstatusBuffer);
	oledUFCstatus.drawStr(24, 7, "DCS F/A-18C");
	oledUFCstatus.sendBuffer();

	displayIP();

	muxSelect(0); // Allow 0x72 to be referenced on channel 0
	drawPixel(i2c_addr_landingGear, 15, 7, LED_ON); displayHT16K33(i2c_addr_landingGear);
	muxSelect(8);
	UFCBrightness.begin();
	UFCVolume1.begin();
	UFCVolume2.begin();
	UFCChannel1.begin();
	UFCChannel1.setValue(0.5);
	UFCChannel2.begin();
	UFCChannel2.setValue(0.5);

#ifndef PERSONAL
	//Create ISR for Encoders
	attachInterrupt(digitalPinToInterrupt(17), UFCBrightnessISR, CHANGE);  //call UFCBrightnessISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(5), UFCVolume1ISR, CHANGE);  //call  UFCVolume1ISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(19), UFCVolume2ISR, CHANGE);  //call  UFCVolume2ISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(12), UFCChannel1ISR, CHANGE);  //call  UFCChannel1ISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(27), UFCChannel2ISR, CHANGE);  //call  UFCChannel2ISR()    when high->low or high->low changes happened
#else
	//Create ISR for Encoders
	attachInterrupt(digitalPinToInterrupt(17), UFCBrightnessISR, CHANGE);  //call UFCBrightnessISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(5), UFCVolume1ISR, CHANGE);  //call  UFCVolume1ISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(19), UFCVolume2ISR, CHANGE);  //call  UFCVolume2ISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(25), UFCChannel1ISR, CHANGE);  //call  UFCChannel1ISR()    when high->low or high->low changes happened
	attachInterrupt(digitalPinToInterrupt(27), UFCChannel2ISR, CHANGE);  //call  UFCChannel2ISR()    when high->low or high->low changes happened
#endif

	UFC_Backlight(i2c_addr_ufc, 16);
	// Dim the UFC backlights
	for (int i = 16; i > 0; i--) {
		UFC_Backlight(i2c_addr_ufc, i);
		delay(200);
	}

}
/// <summary>Main processing loop</summary>
/// <remarks>Polls for UDP data.  Manages periodic functions as well as servicing the key stack and encoders.  This will also flush out any indicator changes.</remarks>
void loop() {
	//if (!connected) {
	//	iotWebConf.doLoop();
	//}
	if (millis() - lastUFCActivityTime > 1800000 | lastUFCActivityTime == 0) {
		ufc_Idle = true;
		idleFunctions();
	}
	if (readSwitches(i2c_addr_ufc) | readSwitches(i2c_addr_landingGear) | ProcessEncoders()) {
		// one or more keys have been placed onto the keystack and need to be processed
		while (keystackCount()>0) {
			//      breath = false;
			keystackPop(kk);
			//  Now send the key info to the serial port
			//  This is done in multiple prints to avoid the use of a string  
			//!specialActions(kk);
			sprintf(replyBuffer, "C%d,%d,\0", kk[2], kk[0]);
			//Serial.println(replyBuffer);
			// note about values:
			// Generally the decade of the value indicates the number of switchcodes (not positions) the switch has.
			// a code in the 40's has 5 switchcodes, in the 10's has 2 switchcodes, in the 0's has 1 switchcode.
			// A three position toggle will have only two switchcodes because the centre position is inferred.
			// The reason for this is to allow us to infer from the value in the table, how many switch codes are
			// associated with the real switch.
			// All of the switchcodes for a real switch are expected to be contiguous in the array.
			// In normal operation, communicating a changed state does not care about the number of switchcodes there are.
			// 
			char tbuf[16];
			switch (kk[1]) {
			case 0:
			case 1:
			case -1:
				// unfortunately the float processing in sprintf for ESP8266 does not work!
				sprintf(tbuf, "%d.00\0", kk[1]);
				break;
			case 10:
			case 11:
			case 12:
				// this handles the tumbler
				sprintf(tbuf, "0.%d0\0", kk[1] - 10);
				break;
			case 40:
			case 41:
			case 42:
			case 43:
			case 44:
				// this handles the rotary switch  
				sprintf(tbuf, "0.%d0\0", kk[1] - 40);
				break;
			case 50:
			case 51:
			case 52:
			case 53:
			case 54:
			case 55:
			case 56:
			case 57:
			case 58:
			case 59:
				// this handles the real encoders which have a float for their value
				// Unfortunately the ESP8266 sprintf does not work for floats due to a bug :-(
				// The value minus 50 is the index into the float array which contains the 
				// encoder value.
				//Serial.println(EncoderValues[kk[1] - 50], DEC);
				dtostrf(EncoderValues[kk[1] - 50], 2, 2, &tbuf[0]);  // this is needed because of the ESP8266 sprintf problem
				EncoderValues[kk[1] - 50] = 0;  // once we've sent the float, set it to zero.
				break;
			default:
				sprintf(tbuf, "%d.00\0", kk[1]);
				break;
			}
			strncat(replyBuffer, tbuf, strlen(tbuf));
			processOutput(replyBuffer);
		}
	}
	yield();
	udpCheck();
	flushHT16K33();
}
/// <summary>Handles the incoming network data</summary>
/// <remarks>ESP32 handles network data asynchronously</remarks>
void udpCheck(void) {
	// This routine is to handle the incoming network data
	//
	// Only Receive data when connected

	if (packetLen > 0) {
		if (ufc_Idle) {
			UFCDisplay.clear();
			displayClear(i2c_addr_ufc);
		}
		ufc_Idle = false;
		lastUFCActivityTime = millis();
		parseCmdPacket(packetBuffer, packetLen);
		packetLen = 0;  // free up the buffer for more data
	}
	if (commandsReady) {
		if (ufc_Idle) {
			UFCDisplay.clear();
			displayClear(i2c_addr_ufc);
		}
		ufc_Idle = false;
		lastUFCActivityTime = millis();
		processOutput(replyBuffer);
	}

}
void parseCmdPacket(char * packetBuffer, int packetLen) {
	for (uint16_t ii = 0; ii<packetLen; ii++) {
		if (packetBuffer[ii] == '*') {  // this removes the SIMID (4 bytes) from the start of the packet
			memcpy(packetBuffer, &packetBuffer[ii + 1], packetLen - ii);
			packetLen = packetLen - ii - 1;
			ii = packetLen;
		}
	}

	for (uint16_t ii = 0; ii<packetLen; ii++) {
		if (packetBuffer[ii] == ':') {
			memcpy(cmdBuffer, packetBuffer, ii);  // move the command into the command buffer
			cmdBuffer[ii] = '\0';
			memcpy(packetBuffer, &packetBuffer[ii + 1], packetLen - ii);  // shift the remaining contents to the start of the buffer
			packetLen = packetLen - ii - 1;
			ii = 0;
			processCmd(cmdBuffer);
		}
		else {
			if (ii == packetLen - 1) {
				memcpy(cmdBuffer, packetBuffer, ii + 1);  // move the command into the command buffer when it wasn't terminated by a :
				cmdBuffer[ii + 1] = '\0';
				packetLen = packetLen - ii;
				ii = packetLen + 1;
				processCmd(cmdBuffer);
			}
		}
	}
}
uint16_t getCmd(char * cmdBuffer) {
	//  This returns the command code from the command buffer ie the bit before the =
	char cmdCodeBuffer[5];
	int cmdLen = strlen(cmdBuffer);
	for (uint16_t ii = 0; ii<cmdLen; ii++) {
		if (cmdBuffer[ii] == '=') {
			memcpy(cmdCodeBuffer, cmdBuffer, ii);  // move the command code into its own string
			cmdCodeBuffer[ii] = '\0';
			return(atoi(cmdCodeBuffer));
		}
	}
	return(9999);
}
float getCmdValfloat(char * cmdBuffer) {
	// This returns the command value if it is a float value, ie the bit after the equals.
	char cmdValBuffer[10];
	int cmdLen = strlen(cmdBuffer);
	for (uint16_t ii = 0; ii<cmdLen; ii++) {
		if (cmdBuffer[ii] == '=') {
			memcpy(cmdValBuffer, &cmdBuffer[ii + 1], cmdLen - ii);  // move the command code into its own string
			cmdValBuffer[cmdLen - ii] = '\0';
			return(atof(cmdValBuffer));
		}
	}
	return(0.000001);
}
int getCmdValint(char * cmdBuffer) {
	// This returns the command value if it is intended to be an integer value, ie the bit after the equals.
	// The input still appears as a floating point number and is cast (which can have problems) on the return.
	char cmdValBuffer[10];
	int cmdLen = strlen(cmdBuffer);
	for (uint16_t ii = 0; ii<cmdLen; ii++) {
		if (cmdBuffer[ii] == '=') {
			memcpy(cmdValBuffer, &cmdBuffer[ii + 1], cmdLen - ii);  // move the command code into its own string
			cmdValBuffer[cmdLen - ii] = '\0';
			return((int)atof(cmdValBuffer));
		}
	}
	return(0);
}
char * getCmdValStr(char * cmdBuffer) {
	// This returns the command value as a string, ie the bit after the equals.
	int cmdLen = strlen(cmdBuffer);
	for (uint16_t ii = 0; ii<cmdLen; ii++) {
		if (cmdBuffer[ii] == '=') {
			memcpy(cmdValStrBuffer, &cmdBuffer[ii + 1], cmdLen - ii);  // move the command code into its own string
			 if (cmdBuffer[ii + 1] == '_')
			{
				cmdBuffer[ii + 1] = '-';
			}
			cmdValStrBuffer[cmdLen - ii] = '\0';
			return(cmdValStrBuffer);
		}
	}
	return("\0");
}
///<summary>Performs processing on a command received over the Network link</summary>
///<remarks>Commands have to be explicitly defined in the switch ladder otherwise they are a no-op</remarks>
void processCmd(char * cmdBuffer) {
	float dialVal = 0;
	int cmdCode = getCmd(cmdBuffer);
	int cmdValue = 0;
	if (cmdCode <2099) {
		cmdValue = getCmdValint(cmdBuffer);
	}
	char *value = getCmdValStr(cmdBuffer);
	//Serial.print("code: "); Serial.print(cmdCode, DEC); Serial.print(" = "); Serial.println(cmdValue, DEC);
	//Serial.print("code: "); Serial.print(cmdCode, DEC); Serial.print(" = "); Serial.println(cmdBuffer);
	switch (cmdCode) {

	case 2095:  // this is the Comm Channel 1 display
	case 2096:  // this is the Comm Channel 2 display
		//Serial.println(value);
		if (value[0] == '\0') {
			cmdValue = 0;
		}
		else if (value[1] == 'G') {
			cmdValue = 21;
		}
		else if (value[1] == 'M') {
			cmdValue = 22;
		}
		else if (value[1] == 'C') {
			cmdValue = 23;
		}
		else if (value[1] == 'S') {
			cmdValue = 24;
		}
		else if (value[1] == 'd') {   // down arrow
			cmdValue = 25;
		}
		else if (value[1] == 'e') {   // up arrow
			cmdValue = 26;
		}
		else {
			//Serial.print("|");Serial.print(value);Serial.println("|");
			if (value[0] == '`')
			{
				cmdBuffer[5] = '1';
			}
			else if (value[0] == '~')
			{
				cmdBuffer[5] = '2';
			}
			else if (value[0] == ' ')
			{
				cmdBuffer[5] = cmdBuffer[6];
				cmdBuffer[6] = '\0';
			}
			cmdValue = getCmdValint(cmdBuffer);
		}
		//Serial.print("cmdValue= ");	Serial.println(cmdValue, DEC);
		drawCharacter(i2c_addr_ufc,cmdCode-2095, UFCchannelNumbers[cmdValue]);
		//displayHT16K33(i2c_addr_ufc);
		break;
	case 2082:  // ODU 1
	case 2083:  // ODU 2
	case 2084:  // ODU 3
	case 2085:  // ODU 4
	case 2086:  // ODU 5
		if (value[0] == '\0') {
			value = "    ";
		}
		UFCDisplay.oduDisplay(cmdCode - 2081, value);
		break;
	case 2087:  // ODU 1 Selected
	case 2088:  // ODU 2 Selected
	case 2089:  // ODU 3 Selected
	case 2090:  // ODU 4 Selected
	case 2091:  // ODU 5 Selected
	{
		uint8_t l = (cmdCode - 2087);
		uint8_t ledAction = LED_OFF;
		if (value[0] == '|') ledAction = LED_ON;
		drawPixel(i2c_addr_ufc, UFCcueing[l], 4, ledAction);
		displayHT16K33(i2c_addr_ufc);
	}
		break;
	case 2092:  // String 1 Main UFC display
	case 2093:  // String 2 Main UFC display
		if (value[0] == '\0') {
			value = "  ";
		} 
		else if (value[0] == '`') {
			value[0] = '1';
		} else if (value[0]== '~'){
			value[0] = '2';
		} else if (value[1] == '_') {
			value = "--";
		}
		UFCDisplay.display(cmdCode-2092,value);
		break;
	case 2094:  // Number Main UFC display
		if (value[0] == '\0') {
			value = "        ";
		} else if (value[0] == '@') {
			value[0] = 'o';
		}
		else if (value[1] == '@') {
			value[1] = 'o';
		}
		UFCDisplay.display(value);
		break;
	case 163:  // HALF_FLAPS Indicator
	case 164:  // FULL_FLAPS Indicator
	case 165:  // Left  Gear Indicator
	case 166:  // Nose  Gear Indicator
	case 167:  // Right Gear Indicator
	{
		uint8_t cmdOffset = 0;
		uint8_t l = (cmdCode - 163 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicators[2][l], generalIndicators[0][l], generalIndicators[1][l], ledAction);
		displayHT16K33(generalIndicators[2][l]);
		muxSelect(8);
	}
	break;
	case 227:  // Landing Gear Lollipop
	{
		uint8_t cmdOffset = 8;
		uint8_t l = (cmdCode - 227 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicators[2][l], generalIndicators[0][l], generalIndicators[1][l], ledAction);
		displayHT16K33(generalIndicators[2][l]);
		muxSelect(8);
	}
	break;
	case 294:  // Hook light
	{
		uint8_t cmdOffset = 7;
		uint8_t l = (cmdCode - 294 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicators[2][l], generalIndicators[0][l], generalIndicators[1][l], ledAction);
		displayHT16K33(generalIndicators[2][l]);
		muxSelect(8);
	} 
	break;
	case 19:  // Speed Brake light
	{
		uint8_t cmdOffset = 6;
		uint8_t l = (cmdCode - 19 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicators[2][l], generalIndicators[0][l], generalIndicators[1][l], ledAction);
		displayHT16K33(generalIndicators[2][l]);
		muxSelect(8);
	}
	break;
	case 23:  // Launch Bar light
	{
		uint8_t cmdOffset = 5;
		uint8_t l = (cmdCode - 23 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicators[2][l], generalIndicators[0][l], generalIndicators[1][l], ledAction);
		displayHT16K33(generalIndicators[2][l]);
		muxSelect(8);
	}
	break;
	// AV-8B Indicators
	case 462:  // AV-8B Nose Gear Warning
	case 463:  // AV-8B Nose Gear Ready
	case 464:  // AV-8B Left Wheel Warning
	case 465:  // AV-8B Left Wheel Ready
	case 466:  // AV-8B Right Wheel Warning
	case 467:  // AV-8B Right Wheel Ready
	case 468:  // AV-8B Main Gear Ready
	case 469:  // AV-8B Main Gear Warning
	{
		uint8_t cmdOffset = 0;
		uint8_t l = (cmdCode - 462 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicatorsAV8B[2][l], generalIndicatorsAV8B[0][l], generalIndicatorsAV8B[1][l], ledAction);
		displayHT16K33(generalIndicatorsAV8B[2][l]);
		muxSelect(8);
	}
	break;
	case 446:  // AV-8B Landing Gear Lollipop
	{
		uint8_t cmdOffset = 8;
		uint8_t l = (cmdCode - 446 + cmdOffset);
		uint8_t ledAction = LED_OFF;
		if (cmdValue == 1) ledAction = LED_ON;
		muxSelect(0);
		drawPixel(generalIndicatorsAV8B[2][l], generalIndicatorsAV8B[0][l], generalIndicatorsAV8B[1][l], ledAction);
		displayHT16K33(generalIndicatorsAV8B[2][l]);
		muxSelect(8);
	}
	break;
	case 65535: // configuration command
	{
		switch (getCmdValint(cmdBuffer)) {
		case 0: {
			ufcType = 0; // F/A-18C
		}
				break;
		case 1: {
			ufcType = 1; // AV-8B
		}
				break;
		default: {
			ufcType = 0; // F/A-18C
		}
			break;
		}
	}
	break;
	default:
		Serial.println("Command Unknown");
		break;
	}
}
///<summary>Sends Device Codes over the network</summary>
///<remarks>Responds to the commandReady boolean</remarks>
void processOutput(char * replyBuffer) {
	Serial.println(replyBuffer);
	udp.writeTo((const uint8_t*)replyBuffer, strlen(replyBuffer),ipRemote,portRemote);
	commandsReady = false;
}
bool readSwitches(uint8_t i2c_address) {
	// Testing suggests that once the HT16K33 switches are read, it takes around 19ms before another interrupt
	// will occur, even if the key is held down for the whole time. This ties up with the HT16K33's 20ms debounce mechanism.
	// Therefore there is no point reading the interrupt flag more frequently than the debounce period.
	// The int flag stays set until all six bytes of key ram has been read, and then it gets reset. 
	// Key data accummulates in RAM until it is read and cleared.
	//
	// this code can be in one of 4 states
	// 1 - no keys are pressed and everything dealt with (this is the normal state)
	// 2 - Interrupt set and one or more keys have been pressed - The interrupt might be set for a new key and we need to spot that an old key has been released
	// 3 - No interrupt, but keys have been recently pressed  - this should mean that all of the keys which have been pressed have now been released
	// 4 - For the CMSC device, there is no interrupt so check for available data also processes it.  This code is for when this happens
	//
	// For this reason, we need to maintain a map of all of the keys which have transitioned from on to off, so that we can raise a keypress event
	// and communicate that the key has now been released.
	// Because we do not get an interrupt for a key being released, we need to remember that there is work to be done for the scan after the last 
	// interrupt.   

	boolean keypressed = false;
	boolean keychanged = false;
	uint8_t keydataavail;
	if (i2c_address == i2c_addr_landingGear) muxSelect(0);
	keydataavail = keydataavailable(i2c_address);
	// Serial.print(" keydataavailable(): "); Serial.print(i2c_address, HEX); Serial.print(" - "); Serial.println(keydataavail, HEX);

	switch (keydataavail) {
	case SWITCHSET:
		memcpy(lastkeys[i2c_address - LOWEST_I2C_ADDR], keys[i2c_address - LOWEST_I2C_ADDR], sizeof(keys[i2c_address - LOWEST_I2C_ADDR])); //save the previous set of keys  
		switch (i2c_address) {
		case i2c_addr_other:
			getOtherSwitchData(i2c_address);
			break;
		default:
			// used for HT16K33's
			getSwitchData(i2c_address);
			break;
		}
		for (uint8_t i = 0; i<6; i++) {
			if (lastkeys[i2c_address - LOWEST_I2C_ADDR][i] != keys[i2c_address - LOWEST_I2C_ADDR][i]) { keychanged = true; break; }
		}
		if (keychanged) {
			for (uint8_t i = 0; i<6; i++) {
				// we are about to do two passes of the data
				// the first is to find keys which have just been pressed
				// the second is to find keys which have just been released
				//
				// New keys being pressed
				uint8_t trans = ~lastkeys[i2c_address - LOWEST_I2C_ADDR][i] & keys[i2c_address - LOWEST_I2C_ADDR][i];  // find out if any keys were newly pressed
				if (trans != 0x00) {
					//Serial.print("Pressed  ");
					//printkeys(i2c_address);
					// we have found at least one key which has been newly pressed ie it was not set last time but is set now.
					for (uint8_t j = 0; j<8; j++) {
						if (0x01 & (trans >> j)) {
							keystackPush((i * 8) + j + 1 + ((i2c_address - 0x70) * 100), true);  //place a keycode onto the stack as pressed - This is the lowest possible address rather than the current lowest.
							keypressed = true;
						}
					}
				}
				// New keys which have been released
				trans = ~keys[i2c_address - LOWEST_I2C_ADDR][i] & lastkeys[i2c_address - LOWEST_I2C_ADDR][i];  // find out if any keys were newly released
				if (trans != 0x00) {
					//Serial.print("Released ");
					//printkeys(i2c_address);
					// we have found at least one key which has been newly released ie it was set last time but is not set now.
					for (uint8_t j = 0; j<8; j++) {
						if (0x01 & (trans >> j)) {
							keystackPush((i * 8) + j + 1 + ((i2c_address - 0x70) * 100), false);  //place a keycode onto the stack as released
							keypressed = true;
						}
					}
				}
			}
		}
		else {
			// this is for when a key is pressed but it is not a new key
			keypressed = false;
		}
		break;
	case SWITCHWAIT:
		// This happens when we are too early to detect an interupt so we will just return reporting that we have
		// not done anything.
		keypressed = false;
		break;
	case SWITCHALREADYDONE:
		// For devices such as some encoders where the cost of the keys is the same as the cost of an interrupt so the keys get stacked in the keydataavailable() routine
		keypressed = true;
		break;
	case SWITCHNONE:
	default:
		// this is most likely that there are no changed keys
		// this is for when we have validly checked an interrupt and none was set.          
		//for(uint8_t i=0;i<6;i++) keys[i2c_address-LOWEST_I2C_ADDR][i] = 0x00;  // rather than do an i2c read, we just set the current keys to 0
		keypressed = false;
		break;
	}
	muxSelect(8);
	return keypressed;  // let caller know that we have some key data
}
void getOtherSwitchData(uint8_t i2c_address) {
	// this function is to read any special switches which have been wired differently
	// the general approach is to only report the transitions, and to make it simpler for 
	// the processing of the key presses, we use a dummy code from an HT16K33
	return;
}
///
int keydataavailable(uint8_t i2c_address) {
	//
	// Testing suggests that once the HT16K33 switches are read, it takes around 19ms before another interrupt
	// will occur, even if the key is held down for the whole time. This ties up with the HT16K33's 20ms debounce mechanism.
	// Therefore there is no point reading the interrupt flag more frequently than the debounce period.
	// The int flag stays set until all six bytes of key ram has been read, and then it gets reset. 
	// Key data accummulates in RAM until it is read and cleared.
	//
	// This routine also attempts to mimic an interrupt for the last key being released so the first
	// time the interrupt flag is checked after it has been set, we will still report that the interrupt
	// has been set to give a chance to push the final releases onto the stack.
	//
	// Toggle switches which are set closed will always present an interrupt rather than just on the transition.
	//
	int retvalue = SWITCHNONE;
	byte interrupt_flag = 0;;
	time1 = micros();                            // only do a call to the clock once
	if (time1 - switchLastIntTime[i2c_address - LOWEST_I2C_ADDR] > 20000) {  // only check the interrupt if it has had time to get set after a debounce period
		switch (i2c_address) {
		case i2c_addr_other:
			// for this pseudo-device there is no interrupt to test and it is multiple devices in reality
			// we want to avoid duplicate work (and using more variables)
			uint8_t tmpkeys[6];                    // get 6 bytes to save the previous keys away to
			memcpy(tmpkeys, keys[i2c_address - LOWEST_I2C_ADDR], sizeof(keys[i2c_address - LOWEST_I2C_ADDR])); //save the previous set of keys  
			getOtherSwitchData(i2c_address);  // this fetches new key values into the keys array, it is a bit wasteful because if there is key action we'll go get it again.
			for (uint8_t i = 0; i<6; i++) {         // we're only really interested in the first byte, but we'll check them all.
				if (keys[i2c_address - LOWEST_I2C_ADDR][i] != tmpkeys[i]) {
					// if we find that there is a change, we raise the interrupt and then restore the previous value             
					interrupt_flag = 0xff;
					memcpy(keys[i2c_address - LOWEST_I2C_ADDR], tmpkeys, sizeof(keys[i2c_address - LOWEST_I2C_ADDR])); // copy back the original key values
					break;  // leave the for loop
				}
			}
			break;
		default:
			// this is for the HT16K33 based devices which have an interrupt to tell us a key has been pressed
			Wire.beginTransmission(i2c_address);
			Wire.write(HT16K33_KEY_INTERUPT_ADDR | 0);
			Wire.endTransmission();
			Wire.requestFrom(i2c_address, (byte)1);    // read the interrupt status
			while (Wire.available()) {                 // slave may send less than requested
				interrupt_flag = Wire.read();          // receive a byte as character
			}
			//if(interrupt_flag & i2c_address == i2c_addr_landingGear){
			//	Serial.print("HT16K33 Interrupt from ");Serial.print(i2c_address,HEX);Serial.print(" : ");Serial.println(interrupt_flag,HEX);
			//}

			break;
		}
		if (interrupt_flag) {
			switchLastIntTime[i2c_address - LOWEST_I2C_ADDR] = time1;   // if the interrupt was set then we need to save the time so that we don't recheck it too quickly
			return SWITCHSET;                                         // this means that there was a valid interrupt
		}
		else {
			// so no interrupt and we've left enough time for it to have occurred.
			// If the interrupt had been pressed on the last pass then we will return an interrupt
			// so that the released keys can be processed, otherwise we simply return a no keys response
			// we use the last interrupt time to determine this.  
			if (switchLastIntTime[i2c_address - LOWEST_I2C_ADDR] == 0) {
				//switchLastIntTime[i2c_address - LOWEST_I2C_ADDR] = time1;  //create delay before triggering again
				return SWITCHNONE;                        // no switch was pressed
			}
			else {
				switchLastIntTime[i2c_address - LOWEST_I2C_ADDR] = 0; // say no dummy interrupt is needed next time
				return SWITCHSET;                                     // say a key was pressed even though none was to allow the released key to be processed
			}
		}
	}
	else {
		// it is too soon for another interrupt to be set so we want to return saying 
		// that it is too short a time since the last interupt to process switch data.
		return SWITCHWAIT;  // Too early
	}
	return true;
}
boolean ProcessEncoders(void) {
	return ProcessOnboardEncoders() || ProcessI2CEncoders();
}
boolean ProcessOnboardEncoders(void) {
/*
	Tests show that the ISR can be triggered without the value of the device changing so
	we do need the double test so that the interrupt flag can be reset even though the 
	value has not changed.
*/
#define  ENC_UFC_BRIGHTNESS 850
#define  ENC_UFC_VOLUME1 851
#define  ENC_UFC_VOLUME2 852
#define  ENC_UFC_CHANNEL1 853
#define  ENC_UFC_CHANNEL2 854

	if (encInt) {
		float tUFCEncoderVal;
		if (encInt & 0b00000001) {
			tUFCEncoderVal = UFCBrightness.getValue();
			encInt &= ~((uint8_t)1 << 0);
			if (tUFCEncoderVal != UFCBrightnessVal) {
				UFCBrightnessVal = tUFCEncoderVal;
				//Serial.print("Brightness value: "); Serial.println(tUFCEncoderVal, DEC);
				keystackPush(ENC_UFC_BRIGHTNESS, true);
				EncoderValues[ENC_UFC_BRIGHTNESS - 850] = UFCBrightnessVal;
				UFC_Backlight(i2c_addr_ufc, int(tUFCEncoderVal*15));
			}
		}
		if (encInt & 0b00000010) {
			tUFCEncoderVal = UFCVolume1.getValue();
			encInt &= ~((uint8_t)1 << 1);
			if (tUFCEncoderVal != UFCVolume1Val) {
				UFCVolume1Val = tUFCEncoderVal;
				//Serial.print("Volume1 value: "); Serial.println(tUFCEncoderVal, DEC);
				keystackPush(ENC_UFC_VOLUME1, true);
				EncoderValues[ENC_UFC_VOLUME1 - 850] = UFCVolume1Val;
			}
		}
		if (encInt & 0b00000100) {
			tUFCEncoderVal = UFCVolume2.getValue();
			encInt &= ~((uint8_t)1 << 2);
			if (tUFCEncoderVal != UFCVolume2Val) {
				UFCVolume2Val = tUFCEncoderVal;
				//Serial.print("Volume2 value: "); Serial.println(tUFCEncoderVal, DEC);
				keystackPush(ENC_UFC_VOLUME2, true);
				EncoderValues[ENC_UFC_VOLUME2 - 850] = UFCVolume2Val;
			}
		}

		if (encInt & 0b00001000) {
			tUFCEncoderVal = UFCChannel1.getValue();
			encInt &= ~((uint8_t)1 << 3);
			if (tUFCEncoderVal != UFCChannel1Val) {
				if (tUFCEncoderVal > UFCChannel1Val) {
					// send 1 for increasing
					EncoderValues[ENC_UFC_CHANNEL1 - 850] = 1;
					//Serial.println("Channel 1 value: Up");
				}
				else {
					// send 0 for decreasing
					EncoderValues[ENC_UFC_CHANNEL1 - 850] = 0;
					//Serial.println("Channel 1 value: Dn");
				}
				//Serial.print("Channel 1 value: "); Serial.println(tUFCEncoderVal,DEC);
				UFCChannel1.setValue(0.5);
				UFCChannel1Val = 0.5;
				keystackPush(ENC_UFC_CHANNEL1, true);
			}
		}
		if (encInt & 0b00010000) {
			tUFCEncoderVal = UFCChannel2.getValue();
			encInt &= ~((uint8_t)1 << 4);
			if (tUFCEncoderVal != UFCChannel2Val) {
				if (tUFCEncoderVal > UFCChannel2Val) {
					// send 1 for increasing
					EncoderValues[ENC_UFC_CHANNEL2 - 850] = 1;
					//Serial.println("Channel 2 value: Up");
				}
				else {
					// send 0 for decreasing
					EncoderValues[ENC_UFC_CHANNEL2 - 850] = 0;
					//Serial.println("Channel 2 value: Dn");
				}
				//Serial.print("Channel 2 value: "); Serial.println(tUFCEncoderVal, DEC);
				UFCChannel2.setValue(0.5);
				UFCChannel2Val = 0.5;
				keystackPush(ENC_UFC_CHANNEL2, true);
			}
		}
		return true;
	}
	else return false;
}
void IRAM_ATTR UFCBrightnessISR()
{
	UFCBrightness.readAB();
	encInt |= 0b00000001;
}
void IRAM_ATTR UFCVolume1ISR()
{
	UFCVolume1.readAB();
	encInt |= 0b00000010;
}
void IRAM_ATTR UFCVolume2ISR()
{
	UFCVolume2.readAB();
	encInt |= 0b00000100;
}
void IRAM_ATTR UFCChannel1ISR()
{
	UFCChannel1.readAB();
	encInt |= 0b00001000;

}
void IRAM_ATTR UFCChannel2ISR()
{
	UFCChannel2.readAB();
	encInt |= 0b00010000;
}
boolean ProcessI2CEncoders(void) {

	/*
	* * * This code handles the polling and reporting of the I2C encoders running AtTiny85's
	* * * for this version of the code, we use the normal keycode mechanism to report the
	* * * values of the encoders, and we do this using value of 50.
	*/
	// these are the keycodes for the encoders and need to appear in the key mappings table
#define  ENC_ILS0  855
#define  ENC_ILS0s 856
#define  ENC_ILS1  857
#define  ENC_ILS1s 858
#define  ENC_TACAN 859
	boolean ret = false;
	return ret;
}
void drawPixel(uint8_t i2c_address, int16_t x, int16_t y, uint16_t color) {
	if ((y < 0) || (y >= 8)) return;
	if ((x < 0) || (x >= 16)) return;

	if (color) {
		displaybuffer[i2c_address - LOWEST_I2C_ADDR][y] |= 1 << x;
	}
	else {
		displaybuffer[i2c_address - LOWEST_I2C_ADDR][y] &= ~(1 << x);
	}
	HT16K33Push[i2c_address - LOWEST_I2C_ADDR] = true;
}
void drawCharacter(uint8_t i2c_address, int16_t x, int16_t y) {
	if ((x < 0) || (x >= 2)) return;
	displaybuffer[i2c_address - LOWEST_I2C_ADDR][x] = y;
}
void displayClear(uint8_t i2c_address) {
	Wire.beginTransmission(i2c_address);
	Wire.write((uint8_t)0x00); // start at address $00
	for (uint8_t i = 0; i<8; i++) {
		displaybuffer[i2c_address - LOWEST_I2C_ADDR][i] = 0;
		Wire.write(displaybuffer[i2c_address - LOWEST_I2C_ADDR][i] & 0xFF);
		Wire.write(displaybuffer[i2c_address - LOWEST_I2C_ADDR][i] >> 8);
	}
	Wire.endTransmission();
}
void getSwitchData(uint8_t i2c_address) {
	// get the six bytes of switch memory from the i2c bus
	uint8_t i;
	if (i2c_address != i2c_addr_landingGear) {
		Wire.beginTransmission(i2c_address);
		Wire.write(HT16K33_KEY_RAM_ADDR);
		Wire.endTransmission();
		Wire.requestFrom(i2c_address, (byte)6);		// try to read the six bytes containing the key presses
		i = 0;
		while (Wire.available()) {           // slave may send less than requested
			keys[i2c_address - LOWEST_I2C_ADDR][i++] = Wire.read();  // receive a byte as character
		}
	}
	else {
		uint8_t tempkeys[2][6];
		delay(25);
		Wire.beginTransmission(i2c_address);
		Wire.write(HT16K33_KEY_RAM_ADDR);
		Wire.endTransmission();
		Wire.requestFrom(i2c_address, (byte)6);		// try to read the six bytes containing the key presses
		i = 0;
		while (Wire.available()) {           // slave may send less than requested
			tempkeys[0][i++] = Wire.read();  // receive a byte as character
		}
		if (i < 5) {
			Serial.print("* * * HT16K33 KEYRAM read less than expected "); Serial.print(" - 0x"); Serial.print(i2c_address, HEX); Serial.print(" - "); Serial.println(i, DEC);
		}
		// this device seems to be a little noisy so we read the key ram a second time
		// and hope that it matches the first read.  If it does not, then we ignore the data
		//
		// This is horribly inefficient, but until the hardware issue is diagnosed and fixed, this is the best
		// we have. 
		delay(25);  // to allow the HT16K33 time to rescan
		Wire.beginTransmission(i2c_address);
		Wire.write(HT16K33_KEY_RAM_ADDR);
		Wire.endTransmission();
		Wire.requestFrom(i2c_address, (byte)6);		// try to read the six bytes containing the key presses
		i = 0;
		while (Wire.available()) {                  // slave may send less than requested
			tempkeys[1][i++] = Wire.read();			// receive a byte as character 
		}
		// We now have two reads which need to be compared
		if (!kequals(tempkeys[0], tempkeys[1])) {

			//Serial.println("0 and 1 are not the same");
			//for (j = 0; j < 2; j++) {
			//	Serial.print("HT16K33 KEYRAM read "); Serial.print(j, DEC); Serial.print(" - "); Serial.print(i2c_address, HEX); Serial.print(" - 0x");
			//	for (i = 0; i < 6; i++) {
			//		Serial.print(tempkeys[j][i] <= 15 ? "0" : "");
			//		Serial.print(tempkeys[j][i], HEX);
			//	}
			//	Serial.println();
			//}
		}
		else {
			for (i = 0; i < 6; i++) {
				keys[i2c_address - LOWEST_I2C_ADDR][i] = tempkeys[0][i];
			}
		}
	}
}
void checkLight(uint8_t i2c_address, int16_t x, int16_t y, uint8_t parm)
{
	if (parm == 1) {
			drawPixel(i2c_address, x, y, LED_ON);
			HT16K33Push[i2c_address - LOWEST_I2C_ADDR] = true;
	}
	else {
			drawPixel(i2c_address, x, y, LED_OFF);
			HT16K33Push[i2c_address - LOWEST_I2C_ADDR] = true;
	}
}
void UFC_Backlight(uint8_t i2c_address,uint8_t UFC_brightness) {
	// the HT16K33 does not turn off the display on brightness 0
	// but we emulate this in this routine.  The input value is
	// 0 = off and 1-16 is a brightness value therefore we
	// must subtract one from the brightness value to get the 
	// correct value for the HT16K33.
	// this is used to turn on all of the UFC backlight LEDs
	if (UFC_brightness > 0) {
		// make sure the display flag is on
		Wire.beginTransmission(i2c_address);
		Wire.write(HT16K33_BLINK_CMD | HT16K33_BLINK_DISPLAYON | (0 << 1));
		Wire.endTransmission();
		UFC_brightness--;
		//Turn on all of the back light LEDs
		for (uint8_t i = 0; i < 12; i++) {
			drawPixel(i2c_address, i, 2, LED_ON);
		}
		for (uint8_t i = 0; i < 8; i++) {
			drawPixel(i2c_address, i, 3, LED_ON);
		}
		displayHT16K33(i2c_address);
		Wire.beginTransmission(i2c_address);
		Wire.write(HT16K33_CMD_BRIGHTNESS | UFC_brightness);
		Wire.endTransmission();
		DLG2416_Brightness(DLG2416_CHANNEL_0, UFC_brightness);
	}
	else {
		// Tuirn off the display flag
		Wire.beginTransmission(i2c_address);
		Wire.write(HT16K33_BLINK_CMD & ~(0xff & HT16K33_BLINK_DISPLAYON) | (0 << 1));
		Wire.endTransmission();
		DLG2416_Brightness(DLG2416_CHANNEL_0, 0);
	}

}
void keystackPush(int16_t key, boolean pressed) {
	boolean ignoreKey = false;
	int16_t kk[3] = { 0,0,0 };  // this is used for the keycode value set
	uint8_t i = 0;
	//test
	//Serial.print("Keycode :  ");Serial.println(key);
	//Serial.print("Pressed key: "); Serial.println(key);  // debug 
	//char msg[32];
	//sprintf(msg, "Key: %3d", key);
	//UFCDisplay.display(0, &msg[0]);
	//UFCDisplay.display(1, &msg[2]);
	//UFCDisplay.display(&msg[4]);
	//end test
	if (convertKey(kk, key)) {  // translate the keycode into it's proper command code and associated settings
		if (pressed) {
			keystack[1][keystackFree] = kk[1];  //this is the value for a pressed key
		}
		else {
			switch (kk[1]) {
			case 0:
				// This handles the key being released  
				keystack[1][keystackFree] = 1;  // this is the reversed value for a released key
				break;
			case 10:
			case 12:
				// This handles the Tumbler switches which return a 10 or 12 so the off is 11 for released  
				keystack[1][keystackFree] = 11;
				break;
			case 13:
			case 15:
				// This handles the Tumbler switches which return a 15 or 13 so the off is 14 for released  
				keystack[1][keystackFree] = 14;
				break;
			case 40:
			case 41:
			case 42:
			case 43:
			case 44:
				//this is a released key for a rotary switch and we just want to ignore it because we have a 
				//switchcode for each position, which allows us to care only about the the pressed case.
				ignoreKey = true;
				break;
			default:
				keystack[1][keystackFree] = 0;  // this is the default value for a released key
				break;
			}
		}
		if (!ignoreKey) {
			keystack[2][keystackFree] = kk[2]; //this is the device for the keycode
			keystack[0][keystackFree++] = kk[0]; //this is the keycode
			if (keystackFree == 64) {
				// this code checks if we're about to fall off the end of the stack and if we are then it compresses the stack
				while (keystackNext <= keystackFree & keystackNext != 0) {
					keystack[2][i] = keystack[2][keystackNext];
					keystack[1][i] = keystack[1][keystackNext];
					keystack[0][i++] = keystack[0][keystackNext++];
				}
				keystackNext = 0;
				keystackFree = i;
			}
		}
	}
	else {
		Serial.print("Missing key: "); Serial.println(key);  // debug    
	}
}
void keystackPop(int16_t *sp) {
	if (keystackNext < keystackFree) {
		sp[0] = keystack[0][keystackNext];
		sp[1] = keystack[1][keystackNext];
		sp[2] = keystack[2][keystackNext++];

		if (keystackNext == keystackFree) {
			// if we have just taken the last entry then set both pointers to the top of the stack
			keystackNext = 0;
			keystackFree = 0;
		}
	}
	else {
		sp[0] = 0;  // indicate no keys
		sp[1] = 0;
		sp[2] = 0;
	}
}
int keystackCount() {
	//returns the number of items on the key stack
	return keystackFree - keystackNext;
}
void keystackPeek(int16_t *sp) {
	//returns the next keycode without removing it from the stack or zero if stack is empty
	if (keystackNext < keystackFree) {
		sp[0] = keystack[0][keystackNext];
		sp[1] = keystack[1][keystackNext];
		sp[2] = keystack[2][keystackNext];
	}
	else {
		sp[0] = 0;  // indicate no keys
		sp[1] = 0;
		sp[2] = 0;
	}
}
boolean convertKey(int16_t *sp, int16_t keycode) {
	// this function returns a pointer to an array containing the key information found from the HT16K33 

	switch (ufcType) {
		case 1: {
			// Serial.println("Converting to AV-8B key.");
			for (uint8_t i = 0; i < FA18KEYMAPPINGS_MAXKEYS && keyMappingsAV8B[0][i] > 0; i++) {
				if (keyMappingsAV8B[0][i] == keycode) {
					sp[0] = keyMappingsAV8B[1][i] + 3000;
					sp[1] = keyMappingsAV8B[2][i];
					sp[2] = keyMappingsAV8B[3][i];
					//Serial.print("ConvertKey(): ");Serial.print(i,DEC);Serial.print(":");Serial.print(sp[0]);Serial.print(":");Serial.print(sp[1]);Serial.print(":");Serial.print(sp[2]);Serial.print(":");Serial.println(keycode);
					return keyMappingsAV8B[4][i] < 9 ? true : false;
				}
			}
		}
		break;
		default:{
			for (uint8_t i = 0; i < FA18KEYMAPPINGS_MAXKEYS && keyMappings[0][i] > 0; i++) {
				if (keyMappings[0][i] == keycode) {
					sp[0] = keyMappings[1][i] + 3000;
					sp[1] = keyMappings[2][i];
					sp[2] = keyMappings[3][i];
					//Serial.print("ConvertKey(): ");Serial.print(i,DEC);Serial.print(":");Serial.print(sp[0]);Serial.print(":");Serial.print(sp[1]);Serial.print(":");Serial.print(sp[2]);Serial.print(":");Serial.println(keycode);
					return keyMappings[4][i] < 9 ? true : false;
				}
			}
		}
		break;
	}
	sp[0] = 0;
	sp[1] = 0;
	sp[2] = 0;
	return false;
}
int16_t realKey(int16_t keycode) {
	// this function returns the index into the keycode array if found or -1 if not found

	for (int16_t i = 0; i< FA18KEYMAPPINGS_MAXKEYS && keyMappings[0][i] > 0; i++) {
		if (keyMappings[0][i] == keycode) {
			return i;
		}
	}
	return -1;
}
void oledUFCMsg(int x, int y, const char * msg) {
	// this routine puts a message on the oled UFC back display to indicate ESP8266 state
	oledUFCstatus.firstPage();
	do {
		oledUFCstatus.setFont(u8g2_font_6x10_tr);
		oledUFCstatus.drawStr(x, y, msg);
	} while (oledUFCstatus.nextPage());
	delay(10);
}
void getDefaultKeys(uint8_t i2c_address) {
	byte interrupt_flag;
	Wire.beginTransmission(i2c_address);
	Wire.write(HT16K33_KEY_RAM_ADDR);
	Wire.endTransmission();
	Wire.requestFrom(i2c_address, (byte)6);  // try to read the six bytes containing the key presses
	Serial.print("{");
	while (Wire.available()) {                 // slave may send less than requested
		interrupt_flag = Wire.read();             // receive a byte as character
		Serial.print("0x");
		Serial.print(interrupt_flag, HEX);
		Serial.print(",");
	}
	Serial.print("},  //");
	Serial.print(i2c_address, HEX);
	Serial.println(" ");

}
/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
	// -- Let IotWebConf test and handle captive portal requests.
	if (iotWebConf.handleCaptivePortal())
	{
		// -- Captive portal request were already served.
		return;
	}
	String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
	s += "<title>BlueFinBima UFC Access Point</title></head><body>Welcome to the Access Point Configurator for the <b>BlueFinBima Up Front Controller</b>.";
	s += "<br/><ul>";
	s += "<li>IP address: ";
	s += ipAddressValue;
	s += "<li>Gateway addr: ";
	s += gatewayValue;
	s += "<li>Net Mask: ";
	s += netmaskValue;
	s += "</ul><br/>";
	s += "Go to <a href='config'>configure page</a> to change settings.";
	s += "</body></html>\n";

	server.send(200, "text/html", s);
}
boolean htmlFormValidator()
{
	Serial.println(F("Validating form."));
	boolean valid = true;

	if (!ipAddress.fromString(server.arg(ipAddressParam.getId())))
	{
		ipAddressParam.errorMessage = "Please provide a valid IP address.";
		valid = false;
	}
	if (!netmask.fromString(server.arg(netmaskParam.getId())))
	{
		netmaskParam.errorMessage = "Please provide a valid netmask.";
		valid = false;
	}
	if (!gateway.fromString(server.arg(gatewayParam.getId())))
	{
		gatewayParam.errorMessage = "Please provide a valid gateway address.";
		valid = false;
	}

	return valid;
}
void iotWebConfConfigSaved()
{
	Serial.println(F("IotWebConf Configuration was updated."));
}
//void connectToWiFi(const char * ssid, const char * pwd) {
//	// This is code to connect to a hard coded address.  Currently not used.
//	Serial.println("Connecting to WiFi network: " + String(ssid));
//
//	// delete old config
//	WiFi.disconnect(true);
//	//register event handler
//	WiFi.onEvent(WiFiEvent);
//
//	//Initiate connection
//	WiFi.config(IPAddress(10, 1, 1, 17), IPAddress(10, 1, 1, 1), IPAddress(255, 255, 255, 0));
//	WiFi.begin(ssid, pwd);
//	Serial.println("Waiting for WIFI connection...");
//	oledUFCMsg(0,32,"Awaiting WiFi");
//}

void connectWifi(const char* ssid, const char* password)
{
	ipAddress.fromString(String(ipAddressValue));
	netmask.fromString(String(netmaskValue));
	gateway.fromString(String(gatewayValue));

	//register event handler
	WiFi.onEvent(WiFiEvent);

	if (!WiFi.config(ipAddress, gateway, netmask)) {
		Serial.println("STA Failed to configure");
	}
	Serial.print("ip: ");
	Serial.println(ipAddress);
	Serial.print("gw: ");
	Serial.println(gateway);
	Serial.print("net: ");
	Serial.println(netmask);
	WiFi.begin(ssid, password);
}
boolean connectAp(const char* apName, const char* password)
{
	// -- Custom AP settings
	return WiFi.softAP(apName, password, 4);
}
//wifi event handler
void WiFiEvent(WiFiEvent_t event) {

	switch (event) {

	case SYSTEM_EVENT_STA_GOT_IP:
		//When connected set
		connected = true;
		Serial.print("WiFi connected! IP address: "); Serial.println(WiFi.localIP());
		ip = WiFi.localIP();
		startUDPListener();
		break;

	case SYSTEM_EVENT_STA_DISCONNECTED:

		Serial.println("WiFi lost connection");
		connected = false;
		break;
	}
}
void startUDPListener(void) {
	if (udp.listen(udpPort)) {
		Serial.print("Listening on port: ");
		Serial.println(udpPort, DEC);
		connected = true;
		udp.onPacket([](AsyncUDPPacket packet) {
			if (packetLen == 0) {
				// if packetLen > 0 then there is data which has not been processed yet
				memcpy(packetBuffer, packet.data(), packet.length());
				packetLen = packet.length();
				portRemote = packet.remotePort();
				ipRemote = packet.remoteIP();
			}
			else {
				//  buffer is locked so we been to wait until the buffer is freed up
			}
		});
	}
}
void testI2CScanner() {
	byte error, address;
	int nDevices;

	Serial.println("Scanning I2C...");

	nDevices = 0;
	for (byte channel = 0; channel < 9; channel++) {
		muxSelect(channel);
		Serial.print("Scanning Channel "); Serial.println(channel, DEC);
		for (address = 1; address < 127; address++)
		{
			// The i2c_scanner uses the return value of
			// the Write.endTransmisstion to see if
			// a device did acknowledge to the address.
			Wire.beginTransmission(address);
			error = Wire.endTransmission();

			if (error == 0)
			{
				Serial.print("I2C device found at address 0x");
				if (address < 16)
					Serial.print("0");
				Serial.print(address, HEX);
				Serial.println("  !");

				nDevices++;
			}
			else if (error == 4)
			{
				Serial.print("Unknow error at address 0x");
				if (address < 16)
					Serial.print("0");
				Serial.println(address, HEX);
			}
		}
	}
	if (nDevices == 0)
		Serial.println("No I2C devices found\n");
	else
		Serial.println("done\n");

	//delay(5000);           // wait 5 seconds for next scan
}
//This is code for the i2c Multiplexer
void muxSelect(byte i) {

	Wire.beginTransmission(i2c_addr_ufc_mux);
	if (i > 7) {
		//request is for a channel greater than the max so turn off the multiplexer
		Wire.write(0);
	}
	else {
		Wire.write(1 << i);
	}
	Wire.endTransmission();
}
void testAll() {
	/* This test code is to ensure that some of the AtTiny85 boards have been defined at the same addess as they were supposed to be */


	// this routine is to test all of the sub-section tests, sets a delay and then turns off the test
	muxSelect(0);
	checkLight(i2c_addr_landingGear, 15, 7, 1);            // turn on the indicator LED on landing gear LHS switch board
	checkLight(i2c_addr_landingGear, 15, 4, 1);            // turn on the indicator LED inside the landing gear lollipod
	displayHT16K33(i2c_addr_landingGear);
	muxSelect(8);
	//checkLight(i2c_addr_rhs, 15, 7, 1);                  // turn on the indicator LED on RHS switch board
	//displayHT16K33(i2c_addr_rhs);

	testGeneral();
	testUFC(i2c_addr_ufc);

	delay(500);
	muxSelect(0);
	displayClear(i2c_addr_landingGear);
	displayHT16K33(i2c_addr_landingGear);
	muxSelect(8);
	//displayClear(i2c_addr_rhs);
	//displayHT16K33(i2c_addr_rhs);
	//displayClear(i2c_addr_cp);
	//displayHT16K33(i2c_addr_cp);
	displayClear(i2c_addr_ufc);
	displayHT16K33(i2c_addr_ufc);

}
void testGeneral(void) {
	// Set various LEDs as defined in an array of oddball i2c LEDs
	muxSelect(0);
	for (uint8_t i = 0; i< NUMBEROFINDICATORS; i++) {
		drawPixel(generalIndicators[2][i], generalIndicators[0][i], generalIndicators[1][i], LED_ON);
		displayHT16K33(generalIndicators[2][i]);
		delay(20);
	}
	muxSelect(8);
}
void testUFC(uint8_t i2c_addr) {
	UFCDisplay.clear();
	//write data
	char msg[] = "F/A-18C Hor net BlueFin Bima2018";
	UFCDisplay.display(0, &msg[0]);
	UFCDisplay.display(1, &msg[2]);
	UFCDisplay.display(&msg[4]);
	UFCDisplay.oduDisplay(1, &msg[12]);
	UFCDisplay.oduDisplay(2, &msg[16]);
	UFCDisplay.oduDisplay(3, &msg[20]);
	UFCDisplay.oduDisplay(4, &msg[24]);
	UFCDisplay.oduDisplay(5, &msg[28]);

	drawPixel(i2c_addr, 10, 2, LED_ON);  // LED on the 0 key

	//for (uint8_t j = 0; j<1; j++) {
	uint8_t k = 24;
	for (uint8_t i = 0; i < 25; i++) {
		drawCharacter(i2c_addr, j, UFCchannelNumbers[i]);
		drawCharacter(i2c_addr, j + 1, UFCchannelNumbers[k--]);
		displayHT16K33(i2c_addr);
		delay(75);
	}
	for (uint8_t i = 0; i < 5; i++) {
		drawPixel(i2c_addr, UFCcueing[i], 4, LED_ON);
		displayHT16K33(i2c_addr);
		delay(50);
	}
	//}

	delay(500);
	for (uint8_t i = 0; i < 2; i++) {
		drawCharacter(i2c_addr, i, 0);
	}
	UFCDisplay.clear();
	displayHT16K33(i2c_addr);
}
void configUFCMessage(void) {
	UFCDisplay.clear();
	//write data
	char msg[] = "AccessPoint   ON 192.168  .4  .1";
	UFCDisplay.display(0, &msg[0]);
	UFCDisplay.display(1, &msg[2]);
	UFCDisplay.display(&msg[4]);
	UFCDisplay.oduDisplay(1, &msg[12]);
	UFCDisplay.oduDisplay(2, &msg[16]);
	UFCDisplay.oduDisplay(3, &msg[20]);
	UFCDisplay.oduDisplay(4, &msg[24]);
	UFCDisplay.oduDisplay(5, &msg[28]);
}
void DLG2416_Brightness(uint8_t channel, uint32_t value) {
	// Arduino like analogWrite
	// value has to be between 0 and valueMax

	// calculate duty, 8191 from 2 ^ 13 - 1
	uint32_t duty = (8191 / 255) * _min(((value + 1) * 16) - 1, 255);

	// write duty to LEDC
	ledcWrite(channel, duty);
}
void displayIP(void) {
	char msg[32];
	sprintf(msg, "Port %5d IP  %3d.%3d.%3d.%3d", udpPort, ip[0], ip[1], ip[2], ip[3]);
	UFCDisplay.display(0, &msg[0]);
	UFCDisplay.display(1, &msg[2]);
	UFCDisplay.display(&msg[4]);
	UFCDisplay.oduDisplay(1, &msg[11]);
	UFCDisplay.oduDisplay(2, &msg[15]);
	UFCDisplay.oduDisplay(3, &msg[19]);
	UFCDisplay.oduDisplay(4, &msg[23]);
	UFCDisplay.oduDisplay(5, &msg[27]);
	delay(2000);
	UFCDisplay.clear();
}
void idleFunctions(void) {
	struct tm timeInfoNow;
	if (getLocalTime(&timeInfoNow)) {
		if (timeInfoNow.tm_sec != timeinfo.tm_sec) {
			char msg[32];
			sprintf(msg, "%02d:%02d.%02d", timeInfoNow.tm_hour, timeInfoNow.tm_min, timeInfoNow.tm_sec);
			UFCDisplay.display(1, msg);
			UFCDisplay.display(0, "  ");
			UFCDisplay.display(&msg[2]);
			UFCDisplay.oduDisplay(1, "LIMA");
			UFCDisplay.oduDisplay(2, "ZULU");
			UFCDisplay.oduDisplay(3, "    ");
			UFCDisplay.oduDisplay(4, "OSET");
			sprintf(msg," %+03d",gmtOffset_sec / 3600);
			UFCDisplay.oduDisplay(5, msg);
			drawPixel(i2c_addr_ufc, UFCcueing[0], 4, LED_ON);
			drawPixel(i2c_addr_ufc, UFCcueing[1], 4, LED_OFF);
			displayHT16K33(i2c_addr_ufc);
			memcpy(&timeinfo, &timeInfoNow, sizeof(struct tm));
		}
	}
	if (!ufc_Idle) {
		UFCDisplay.clear();
		drawPixel(i2c_addr_ufc, UFCcueing[0], 4, LED_OFF);
		drawPixel(i2c_addr_ufc, UFCcueing[1], 4, LED_OFF);
		displayHT16K33(i2c_addr_ufc);
	}
}
void printkeys(uint8_t i2c_address) {
	Serial.print("printkeys("); Serial.print(i2c_address, HEX); Serial.print(") - ");
	for (i = 0; i < 6; i++) {
		Serial.print(keys[i2c_address - LOWEST_I2C_ADDR][i] < 16 ? "0" : "");
		Serial.print(keys[i2c_address - LOWEST_I2C_ADDR][i], HEX);
	}
	Serial.println();
}
bool kequals(uint8_t a[], uint8_t b[]) {
	bool test = true;
	for (i = 0; i < 6; i++) {
		if (a[i] != b[i]) test = false;
	}
	return test;
}
/// <summary>Save LED status to the HT16K33 chips which need it</summary>
/// <remarks>There is a boolean array which indicates which HT16K33 chips have had LEDs set or reset and need their display data sent to them.</remarks> 
void flushHT16K33(void) {
	// Flush out the LED changes to the HT16K33 devices which need it
	for (i = 0; i < HIGHEST_I2C_ADDR - LOWEST_I2C_ADDR; i++) {
		if (HT16K33Push[i]) {
			if (i == 1) muxSelect(0);
			displayHT16K33(LOWEST_I2C_ADDR + i);
			HT16K33Push[i] = false;
			if (i == 1) muxSelect(8);
		}
	}
}
void displayHT16K33(uint8_t i2c_address)
{
	Wire.beginTransmission(i2c_address);
	Wire.write((uint8_t)0x00); // start at address $00
	for (uint8_t i = 0; i < 8; i++) {
		Wire.write(displaybuffer[i2c_address - LOWEST_I2C_ADDR][i] & 0xFF);
		Wire.write(displaybuffer[i2c_address - LOWEST_I2C_ADDR][i] >> 8);
	}
	Wire.endTransmission();
}
void initHT16K33(uint8_t i2c_address)
{
	//Wire.begin();
	Wire.beginTransmission(i2c_address);
	Wire.write(0x21);  // turn on oscillator
	Wire.endTransmission();
	Wire.beginTransmission(i2c_address);
	Wire.write(HT16K33_BLINK_CMD | HT16K33_BLINK_DISPLAYON | (0 << 1));
	Wire.endTransmission();
	Wire.beginTransmission(i2c_address);
	Wire.write(HT16K33_CMD_BRIGHTNESS | HT16K33_DEFAULT_BRIGHTNESS);
	Wire.endTransmission();
	Wire.beginTransmission(i2c_address);
	Wire.write(HT16K33_KEY_ROWINT_ADDR | 0);
	Wire.endTransmission();
	displayClear(i2c_address);
}