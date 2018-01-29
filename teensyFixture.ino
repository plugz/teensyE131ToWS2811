// E1.31 Receiver code base by Andrew Huxtable (andrew@hux.net.au).Adapted and modified by
// ccr (megapixel.lighting).
//
// This code may be freely distributed and used as you see fit for non-profit
// purposes and as long as the original author is credited and it remains open
// source
//
// Please configure your Lighting product to use Unicast to the IP the device is given from your
// DHCP server Multicast is not currently supported due to bandwidth/processor limitations

// You will need the Ethercard and FastLed Libraries from:
// [url]https://github.com/FastLED/FastLED/releases[/url]
//
// The Atmega328 only has 2k of SRAM.  This is a severe limitation to being able to control large
// numbers of smart pixels due to the pixel data needing to be stored in an array as well as
// a reserved buffer for receiving Ethernet packets.  This code will allow you to use a maximum of
// 240 pixels as that just about maxes out the SRAM on the Atmega328.

// There is deliberately no serial based reporting from the code to conserve SRAM.  There is a
// **little** bit available if you need to add some in for debugging but keep it to an absolute
// minimum for debugging only.

#include "Log.hpp"
#include "Utils.hpp"

// clang-format off
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#define USE_OCTOWS2811
#include <OctoWS2811.h>
#include <FastLED.h>
// clang-format on

// enter desired universe and subnet  (sACN first universe is 1)
#define DMX_SUBNET 0
#define DMX_UNIVERSE 1 //**Start** universe

// Set a different MAC address for each...
byte mac[] = {0x74, 0x69, 0x69, 0x2D, 0x30, 0x15};
IPAddress ip(192, 168, 2, 2); // IP address of ethernet shield
EthernetUDP Udp;
#define ETHERNET_BUFFER 636 // 540

/// DONT CHANGE unless you know the consequences...
#define CHANNEL_COUNT 4800 // because it divides by 3 nicely
#define NUM_LEDS 8         // can not go higher than this - Runs out of SRAM
#define NUM_LEDS_PER_STRIP 8
#define NUM_STRIPS 1
#define UNIVERSE_COUNT 1
#define LEDS_PER_UNIVERSE 170

// MAX VALUES: 32 universes (~25 fps ?)
// #define CHANNEL_COUNT 16320 //because it divides by 3 nicely
// #define NUM_LEDS 5440 // can not go higher than this - Runs out of SRAM
// #define NUM_LEDS_PER_STRIP 680
// #define NUM_STRIPS 8
// #define UNIVERSE_COUNT 32
// #define LEDS_PER_UNIVERSE 170

// 0-255
#define BRIGHTNESS 255

// Define the array of leds
CRGB leds[NUM_STRIPS * NUM_LEDS_PER_STRIP];

unsigned char packetBuffer[ETHERNET_BUFFER];
int c = 0;
float fps = 0;
unsigned long currentMillis = 0;
unsigned long previousMillis = 0;

void flashLeds() {
    LEDS.showColor(CRGB(255, 0, 0)); // turn all pixels on red
    delay(500);
    LEDS.showColor(CRGB(0, 255, 0)); // turn all pixels on green
    delay(500);
    LEDS.showColor(CRGB(0, 0, 255)); // turn all pixels on blue
    delay(500);
    LEDS.showColor(CRGB(0, 0, 0)); // turn all pixels off
}

void setup() {
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW); // reset the WIZ820io
    delay(10);
    digitalWrite(9, HIGH); // reset the WIZ820io

    LOGSETUP();

    // *** WARNING ***
    // 3 big blocks of Memory in use here:
    // The first is your LEDs array - which is going to be over NUM_LEDS_PER_STRIP * NUM_STRIPS * 3
    // bytes. The second is the buffer that octows2811 is writing out of, which is the same size as
    // above. The third is a buffer that is used to translate/scale the first buffer while
    // octows2811 may still be writing data out of the second buffer. The Teensy 3.2 has only 64K to
    // work with.
    LEDS.addLeds<OCTOWS2811>(leds, NUM_LEDS_PER_STRIP);
    LEDS.setBrightness(BRIGHTNESS);

    // static ip
    Ethernet.begin(mac, ip);
    // dhcp
    // int status;
    // do {
    //    status = Ethernet.begin(mac);
    //    LOG_DEBUG("ethernet status: ");
    //    LOGLN_DEBUG(status);
    //} while (status == 0);
    Udp.begin(5568);
    LOG_DEBUG("IP: ");
    LOGLN_DEBUG(Ethernet.localIP());

    // Once the Ethernet is initialised, run a test on the LEDs
    flashLeds();
}

void sacnDMXReceived(unsigned char *pbuff, int count, int unicount) {
    if (count > CHANNEL_COUNT)
        count = CHANNEL_COUNT;
    byte b = pbuff[113]; // DMX Subnet
    if (b == DMX_SUBNET) {
        LOGLN_DEBUG("subnet OK");

        b = pbuff[114]; // DMX Universe

        if (b >= DMX_UNIVERSE && b <= DMX_UNIVERSE + UNIVERSE_COUNT) {
            LOGLN_DEBUG("universe OK");

            if (pbuff[125] == 0) // start code must be 0
            {
                LOGLN_DEBUG("startCode OK");
                unsigned int ledNumber = (b - DMX_UNIVERSE) * LEDS_PER_UNIVERSE;
                int valueR = pbuff[0];
                int valueG = pbuff[1];
                int valueB = pbuff[2];
                for (int i = 126 + 3; (i < 126 + 3 + count) && (ledNumber < ARRAY_COUNT(leds));
                     ++i && ++ledNumber) {
                    int brightness = pbuff[i];
                    leds[ledNumber] = CRGB((valueR * brightness) / 255,
                            (valueG * brightness) / 255,
                                           (valueB * brightness) / 255);
                    LOG_DEBUG("Led ", DEC);
                    LOG_DEBUG(ledNumber, DEC);
                    LOG_DEBUG(": ");
                    LOG_DEBUG((valueR * brightness) / 255, DEC);
                    LOG_DEBUG(", ");
                    LOG_DEBUG((valueG * brightness) / 255, DEC);
                    LOG_DEBUG(", ");
                    LOG_DEBUG((valueB * brightness) / 255, DEC);
                    LOGLN_DEBUG();
                }

                LOGLN_DEBUG(unicount);
                if (unicount == UNIVERSE_COUNT) {
                    LEDS.show();
                }
            }
        }
    }
}

int checkACNHeaders(unsigned char *messagein, int messagelength) {
    // Do some VERY basic checks to see if it's an E1.31 packet.
    // Bytes 4 to 12 of an E1.31 Packet contain "ACN-E1.17"
    // Only checking for the A and the 7 in the right places as well as 0x10 as the header.
    // Technically this is outside of spec and could cause problems but its enough checks for us
    // to determine if the packet should be tossed or used.
    // This improves the speed of packet processing as well as reducing the memory overhead.
    // On an Isolated network this should never be a problem....
    if (messagein[1] == 0x10 && messagein[4] == 0x41 && messagein[12] == 0x37) {
        // number of values plus start code
        int addresscount = (byte)messagein[123] * 256 + (byte)messagein[124];
        return addresscount - 1; // Return how many values are in the packet.
    }
    return 0;
}

void loop() {
    // Process packets
    int packetSize = Udp.parsePacket(); // Read UDP packet count
    if (c >= 10) {
        c = 0;
    }
    if (packetSize > 0) {
        LOGLN_DEBUG(packetSize);
        LOGLN_DEBUG("reading");
        Udp.read(packetBuffer, ETHERNET_BUFFER); // read UDP packet
        LOGLN_DEBUG("read done");
        int count = checkACNHeaders(packetBuffer, packetSize);
        LOGLN_DEBUG("check done");
        if (count) {
            LOG_DEBUG("packet size first ");
            LOGLN_DEBUG(packetSize);
            c = c + 1;
            // calculate framerate
            currentMillis = millis();
            if (currentMillis > previousMillis) {
                fps = 1 / ((currentMillis - previousMillis) * 0.001);
            } else {
                fps = 0;
            }
            previousMillis = currentMillis;
            if (fps > 10 && fps < 500) // don't show numbers below or over given ammount
                LOGLN_DEBUG(fps);
            sacnDMXReceived(packetBuffer, count, c); // process data function
        } else
            LOGLN_DEBUG("not sacn");
    }
}