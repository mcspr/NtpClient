/*
Copyright 2016 German Martin (gmag11@gmail.com). All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met :

1. Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list
of conditions and the following disclaimer in the documentation and / or other materials
provided with the distribution.

THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ''AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.IN NO EVENT SHALL <COPYRIGHT HOLDER> OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT(INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those of the
authors and should not be interpreted as representing official policies, either expressed
or implied, of German Martin
*/
//
//
//

#include "NtpClientLib.h"

#define DBG_PORT Serial

#ifdef DEBUG_NTPCLIENT
#define DEBUGLOG(...) DBG_PORT.printf(__VA_ARGS__)
#else
#define DEBUGLOG(...)
#endif


NTPClient::NTPClient () {
}

bool NTPClient::setNtpServerName (String ntpServerName) {
    char * name = (char *)malloc ((ntpServerName.length () + 1) * sizeof (char));
    if (!name)
        return false;
    ntpServerName.toCharArray (name, ntpServerName.length () + 1);
    DEBUGLOG ("NTP server set to %s\n", name);
    free (_ntpServerName);
    _ntpServerName = name;
    return true;
}

bool NTPClient::setNtpServerName (char* ntpServerName) {
    char *name = ntpServerName;
    if (name == NULL)
        return false;
    DEBUGLOG ("NTP server set to %s\n", name);
    free (_ntpServerName);
    _ntpServerName = name;
    return true;
}

String NTPClient::getNtpServerName () {
    return String (_ntpServerName);
}

char* NTPClient::getNtpServerNamePtr () {
    return _ntpServerName;
}

bool NTPClient::setDSTZone (uint8_t dstZone) {
    if (dstZone < DST_ZONE_COUNT) {
        _dstZone = dstZone;
        return true;
    }
    return false;
}

uint8_t NTPClient::getDSTZone () {
    return _dstZone;
}

bool NTPClient::setTimeZone (int8_t timeZone, int8_t minutes) {
    if ((timeZone >= -12) && (timeZone <= 14) && (minutes >= -59) && (minutes <= 59)) {
        // Do the maths to change current time, but only if we are not yet sync'ed,
        // we don't want to trigger the UDP query with the now() below
        if (_lastSyncd > 0) {
            int8_t timeDiff = timeZone - _timeZone;
            int8_t minDiff = minutes - _minutesOffset;
            setTime (now () + timeDiff * SECS_PER_HOUR + minDiff * SECS_PER_MIN);
        }
        _timeZone = timeZone;
        _minutesOffset = minutes;
        DEBUGLOG ("NTP time zone set to: %d\r\n", timeZone);
        return true;
    }
    return false;
}

boolean sendNTPpacket (IPAddress address, UDP *udp) {
    uint8_t ntpPacketBuffer[NTP_PACKET_SIZE]; //Buffer to store request message

                                           // set all bytes in the buffer to 0
    memset (ntpPacketBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    ntpPacketBuffer[0] = 0b11100011;   // LI, Version, Mode
    ntpPacketBuffer[1] = 0;     // Stratum, or type of clock
    ntpPacketBuffer[2] = 6;     // Polling Interval
    ntpPacketBuffer[3] = 0xEC;  // Peer Clock Precision
                                // 8 bytes of zero for Root Delay & Root Dispersion
    ntpPacketBuffer[12] = 49;
    ntpPacketBuffer[13] = 0x4E;
    ntpPacketBuffer[14] = 49;
    ntpPacketBuffer[15] = 52;
    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    udp->beginPacket (address, DEFAULT_NTP_PORT); //NTP requests are to port 123
    udp->write (ntpPacketBuffer, NTP_PACKET_SIZE);
    udp->endPacket ();
    return true;
}

time_t NTPClient::getTime () {
    IPAddress timeServerIP; //NTP server IP address
    uint8_t ntpPacketBuffer[NTP_PACKET_SIZE]; //Buffer to store response message


    DEBUGLOG ("Starting UDP\n");
    udp->begin (DEFAULT_NTP_PORT);
    //DEBUGLOG ("UDP port: %d\n",udp->localPort());
    while (udp->parsePacket () > 0); // discard any previously received packets
#if NETWORK_TYPE == NETWORK_W5100
    DNSClient dns;
    dns.begin (Ethernet.dnsServerIP ());
    int8_t dnsResult = dns.getHostByName (getNtpServerName ().c_str (), timeServerIP);
    if (dnsResult <= 0) {
        if (onSyncEvent)
            onSyncEvent (invalidAddress);
        return 0; // return 0 if unable to get the time
    }
#else
    WiFi.hostByName (getNtpServerName ().c_str (), timeServerIP);
#endif
    DEBUGLOG ("NTP Server IP: %s\r\n", timeServerIP.toString ().c_str ());
    sendNTPpacket (timeServerIP, udp);
    uint32_t beginWait = millis ();
    while (millis () - beginWait < ntpTimeout) {
        int size = udp->parsePacket ();
        if (size >= NTP_PACKET_SIZE) {
            DEBUGLOG ("-- Received NTP Response, size:%u\n", size);
            udp->read (ntpPacketBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
            time_t timeValue = decodeNtpMessage (ntpPacketBuffer);
            if (timeValue == 0) {
                break;
            }

            setSyncInterval (getLongInterval ());
            if (!_firstSync) {
                //    if (timeStatus () == timeSet)
                _firstSync = timeValue;
            }
            //getFirstSync (); // Set firstSync value if not set before
            DEBUGLOG ("Sync frequency set low\n");
            udp->stop ();
            setLastNTPSync (timeValue);
            DEBUGLOG ("Successful NTP sync at %s\n", getTimeDateString (getLastNTPSync ()).c_str ());

            if (onSyncEvent)
                onSyncEvent (timeSyncd);
            return timeValue;
        }
#ifdef ARDUINO_ARCH_ESP8266
        ESP.wdtFeed ();
        yield ();
#endif
    }
    DEBUGLOG ("-- No NTP Response :-(\n");
    udp->stop ();
    setSyncInterval (getShortInterval ()); // Retry connection more often
    if (onSyncEvent)
        onSyncEvent (noResponse);
    return 0; // return 0 if unable to get the time
}

int8_t NTPClient::getTimeZone () {
    return _timeZone;
}

int8_t NTPClient::getTimeZoneMinutes () {
    return _minutesOffset;
}

/*void NTPClient::setLastNTPSync(time_t moment) {
    _lastSyncd = moment;
}*/

time_t NTPClient::s_getTime () {
    return NTP.getTime ();
}

#if NETWORK_TYPE == NETWORK_W5100
bool NTPClient::begin (String ntpServerName, int8_t timeZone, bool daylight, int8_t minutes, EthernetUDP* udp_conn) {
#elif NETWORK_TYPE == NETWORK_ESP8266 || NETWORK_TYPE == NETWORK_WIFI101 || NETWORK_TYPE == NETWORK_ESP32
bool NTPClient::begin (String ntpServerName, int8_t timeZone, bool daylight, int8_t minutes, WiFiUDP* udp_conn) {
#endif
    if (!setNtpServerName (ntpServerName)) {
        DEBUGLOG ("Time sync not started\r\n");
        return false;
    }
    if (!setTimeZone (timeZone, minutes)) {
        DEBUGLOG ("Time sync not started\r\n");
        return false;
    }

    if (udp_conn) {
        udp = udp_conn;
    }

    if (!udp) {
#if NETWORK_TYPE == NETWORK_W5100
        udp = new EthernetUDP ();
#else
        udp = new WiFiUDP ();
#endif
    }

    //_timeZone = timeZone;
    setDayLight (daylight);
    _lastSyncd = 0;

    if (_shortInterval == 0 && _longInterval == 0) {
        if (!setInterval (DEFAULT_NTP_SHORTINTERVAL, DEFAULT_NTP_INTERVAL)) {
            DEBUGLOG ("Time sync not started\r\n");
            return false;
        }
    }
    DEBUGLOG ("Time sync started\r\n");

    setSyncInterval (getShortInterval ());
    setSyncProvider (s_getTime);

    return true;
}

bool NTPClient::stop () {
    setSyncProvider (NULL);
    DEBUGLOG ("Time sync disabled\n");

    return true;
}

bool NTPClient::setInterval (int interval) {
    if (interval >= 10) {
        if (_longInterval != interval) {
            _longInterval = interval;
            DEBUGLOG ("Sync interval set to %d\n", interval);
            if (timeStatus () == timeSet)
                setSyncInterval (interval);
        }
        return true;
    } else
        return false;
}

bool NTPClient::setInterval (int shortInterval, int longInterval) {
    if (shortInterval >= 10 && longInterval >= 10) {
        _shortInterval = shortInterval;
        _longInterval = longInterval;
        if (timeStatus () != timeSet) {
            setSyncInterval (shortInterval);
        } else {
            setSyncInterval (longInterval);
        }
        DEBUGLOG ("Short sync interval set to %d\n", shortInterval);
        DEBUGLOG ("Long sync interval set to %d\n", longInterval);
        return true;
    } else
        return false;
}

int NTPClient::getInterval () {
    return _longInterval;
}

int NTPClient::getShortInterval () {
    return _shortInterval;
}

void NTPClient::setDayLight (bool daylight) {

    // Do the maths to change current time, but only if we are not yet sync'ed,
    // we don't want to trigger the UDP query with the now() below
    if (_lastSyncd > 0) {
        if ((_daylight != daylight) && isSummerTimePeriod (now ())) {
            if (daylight) {
                setTime (now () + SECS_PER_HOUR);
            } else {
                setTime (now () - SECS_PER_HOUR);
            }
        }
    }

    _daylight = daylight;
    DEBUGLOG ("--Set daylight saving %s\n", daylight ? "ON" : "OFF");

}

bool NTPClient::getDayLight () {
    return _daylight;
}

String NTPClient::getTimeStr (time_t moment) {
    char timeStr[10];
    sprintf (timeStr, "%02d:%02d:%02d", hour (moment), minute (moment), second (moment));

    return timeStr;
}

String NTPClient::getDateStr (time_t moment) {
    char dateStr[12];
    sprintf (dateStr, "%02d/%02d/%4d", day (moment), month (moment), year (moment));

    return dateStr;
}

String NTPClient::getTimeDateString (time_t moment) {
    return getTimeStr (moment) + " " + getDateStr (moment);
}

time_t NTPClient::getLastNTPSync () {
    return _lastSyncd;
}

void NTPClient::onNTPSyncEvent (onSyncEvent_t handler) {
    onSyncEvent = handler;
}

time_t NTPClient::getUptime () {
    _uptime = _uptime + (millis () - _uptime);
    return _uptime / 1000;
}

String NTPClient::getUptimeString () {
    uint16_t days;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;

    time_t uptime = getUptime ();

    seconds = uptime % SECS_PER_MIN;
    uptime -= seconds;
    minutes = (uptime % SECS_PER_HOUR) / SECS_PER_MIN;
    uptime -= minutes * SECS_PER_MIN;
    hours = (uptime % SECS_PER_DAY) / SECS_PER_HOUR;
    uptime -= hours * SECS_PER_HOUR;
    days = uptime / SECS_PER_DAY;

    char uptimeStr[20];
    sprintf (uptimeStr, "%4u days %02d:%02d:%02d", days, hours, minutes, seconds);

    return uptimeStr;
}

time_t NTPClient::getLastBootTime () {
    if (timeStatus () == timeSet) {
        return (now () - getUptime ());
    }
    return 0;
}

time_t NTPClient::getFirstSync () {
    /*if (!_firstSync) {
        if (timeStatus () == timeSet) {
            _firstSync = now () - getUptime ();
        }
    }*/
    return _firstSync;
}

bool NTPClient::summertime (int year, byte month, byte day, byte hour, byte weekday, byte tzHours)
// input parameters: "normal time" for year, month, day, hour, weekday and tzHours (0=UTC, 1=MEZ)
{
    if (DST_ZONE_EU == _dstZone) {
        if ((month < 3) || (month > 10)) return false; // keine Sommerzeit in Jan, Feb, Nov, Dez
        if ((month > 3) && (month < 10)) return true; // Sommerzeit in Apr, Mai, Jun, Jul, Aug, Sep
        if (month == 3 && ((hour + 24 * day) >= (1 + tzHours + 24 * (31 - (5 * year / 4 + 4) % 7))) || (month == 10 && (hour + 24 * day) < (1 + tzHours + 24 * (31 - (5 * year / 4 + 1) % 7))))
            return true;
        else
            return false;
    }

    if (DST_ZONE_USA == _dstZone) {

        // always false for Jan, Feb and Dec
        if ((month < 3) || (month > 11)) return false;

        // always true from Apr to Oct
        if ((month > 3) && (month < 11)) return true;

        // first sunday of current month
        uint8_t first_sunday = (7 + day - weekday) % 7 + 1;

        // Starts at 2:00 am on the second sunday of Mar
        if (3 == month) {
            if (day < 7 + first_sunday) return false;
            if (day > 7 + first_sunday) return true;
            return (hour > 2);
        }

        // Ends a 2:00 am on the first sunday of Nov
        // We are only getting here if its Nov
        if (day < first_sunday) return true;
        if (day > first_sunday) return false;
        return (hour < 2);

    }

}

boolean NTPClient::isSummerTimePeriod (time_t moment) {
    return summertime (year (), month (), day (), hour (), weekday (), getTimeZone ());
}

void NTPClient::setLastNTPSync (time_t moment) {
    _lastSyncd = moment;
}

uint16_t NTPClient::getNTPTimeout () {
    return ntpTimeout;
}

boolean NTPClient::setNTPTimeout (uint16_t milliseconds) {

    if (milliseconds >= MIN_NTP_TIMEOUT) {
        ntpTimeout = milliseconds;
        DEBUGLOG ("Set NTP timeout to %u ms\n", milliseconds);
        return true;
    }
    DEBUGLOG ("NTP timeout should be higher than %u ms. You've tried to set %u ms\n", MIN_NTP_TIMEOUT, milliseconds);
    return false;

}

// @mcspr:
// Extended checks for packet validity:
// - stratum value
// - non-zero transmit timestamp
// - additional checks from https://github.com/ropg/ezTime/pull/33
//   compare reference and receive timestamps, make sure they are in chronological order

// --- First 32 bits ---
// Leap Indicator: 2 bits
// Version number: 3 bits
// Mode:           3 bits
// Stratum:        8 bits (0 invalid, 1 primary, 2-15 generic, 16 unsyncronized, 17..255 reserved)
// Poll interval:  8 bits (signed, log2 seconds)
// Precision:      8 bits (signed, log2 seconds)
// ...
// --- Last 192 bits ---
// Reference ts:   32 bits (unsigned, seconds since 1900)
// ts fraction:    32 bits (unsigned, not used here)
// Receive ts:     32 bits (unsigned, seconds since 1900)
// ts fraction:    32 bits (unsigned, not used here)
// Transmit ts:    32 bits (unsigned, seconds since 1900)
// ts Fraction:    32 bits (unsigned, not used here)
time_t NTPClient::decodeNtpMessage (uint8_t *messageBuffer) {

    // Discard invalid stratum values
    unsigned long stratum = messageBuffer[1];
    if ((stratum == 0) || (stratum >= 16)) {
        DEBUGLOG ("-- ERROR: NTP packet stratum is invalid\n");
        return 0;
    }

    uint32_t high, low;

    // reference timestamp
    high = (messageBuffer[16] << 8 | messageBuffer[17]) & 0x0000FFFF;
    low = (messageBuffer[18] << 8 | messageBuffer[19]) & 0x0000FFFF;
    uint32_t reference = high << 16 | low;

    // receive timestamp
    high = (messageBuffer[32] << 8 | messageBuffer[33]) & 0x0000FFFF;
    high = (messageBuffer[34] << 8 | messageBuffer[35]) & 0x0000FFFF;
    uint32_t receive = high << 16 | low;

    // transmit timestamp (<-- we are using this)
    high = (messageBuffer[40] << 8 | messageBuffer[41]) & 0x0000FFFF;
    low = (messageBuffer[42] << 8 | messageBuffer[43]) & 0x0000FFFF;
    uint32_t secsSince1900 = high << 16 | low;

    if ((reference == 0) or (receive == 0) or (receive > secsSince1900)) {
        DEBUGLOG ("-- ERROR: NTP timestamps are invalid! reference:%u receive:%u transmit:%u\n",
                   reference, receive, secsSince1900);
        return 0;
    }

    if (secsSince1900 == 0) {
        DEBUGLOG ("-- ERROR: NTP packet timestamp is 0\n");
        return 0;
    }

#define SEVENTY_YEARS 2208988800UL
    time_t timeTemp = secsSince1900 - SEVENTY_YEARS + _timeZone * SECS_PER_HOUR + _minutesOffset * SECS_PER_MIN;

    if (_daylight) {
        if (summertime (year (timeTemp), month (timeTemp), day (timeTemp), hour (timeTemp), weekday (timeTemp), _timeZone)) {
            timeTemp += SECS_PER_HOUR;
            DEBUGLOG ("Summer Time\n");
        } else {
            DEBUGLOG ("Winter Time\n");
        }
    } else {
        DEBUGLOG ("No daylight\n");
    }
    return timeTemp;
}

NTPClient NTP;
