#ifndef CONNECTIVITY_H
#define CONNECTIVITY_H

#include <Arduino.h>

// CONNECTIVITY MODULE
// SIM800L GSM/GPRS module management via AT commands over UART2.
// Handles power-on/recovery, network registration, GPRS bearer, HTTP POST
// upload to the backend, direct SMS (both backend-triggered failsafe alerts
// and the offline local-threshold failsafe), and NTP time retrieval for the
// RTC module.

// Bring up UART2 to the SIM800L and get it responding to AT commands.
// Powers the module on via PWRKEY if it isn't already responding, retrying
// up to SIM_BOOT_RETRY_ATTEMPTS times. Call once per wake cycle before any
// other connectivity function.
void simInit();

// Blocks until the module is responding to "AT" with "OK", power-cycling via
// PWRKEY if needed. Returns false if the module is still unresponsive after
// SIM_BOOT_RETRY_ATTEMPTS attempts.
bool simWaitBoot();

// Checks GSM network registration (AT+CREG?). Returns true if registered,
// home or roaming.
bool simNetworkRegistered();

// Returns signal quality 0-31 (higher is better), or 99 if unknown/no signal.
int simSignalQuality();

// Opens the GPRS data bearer (AT+SAPBR). Must be called after
// simNetworkRegistered() returns true. Returns true if an IP was assigned.
bool gprsConnect();

// Closes the GPRS data bearer.
void gprsDisconnect();

// POSTs a JSON payload to SERVER_URL + API_ENDPOINT over the open GPRS
// bearer, with retry and exponential backoff. Returns true on a 200 response.
bool httpPost(const char* jsonPayload);

// Sends an SMS with the given message body to every non-empty number in
// SMS_NUMBER_1 / _2 / _3. Returns true if it was sent to at least one number.
bool smsSendAlert(const char* message);

// Retrieves current time from the network via AT+CNTP / AT+CCLK, over an
// already-open GPRS bearer. Writes the Unix timestamp to *unixTimeOut.
// Returns false if the module doesn't respond or the response can't be
// parsed. Used by rtc_module.cpp for first-boot / periodic NTP sync.
bool simGetNetworkTime(uint32_t* unixTimeOut);

// Drops the SIM800L into low-power mode (AT+CFUN=0) before deep sleep.
// The module must be re-initialised with simInit()/simWaitBoot() (which
// sends AT+CFUN=1 first) on the next wake cycle before use.
void simSleep();

#endif // CONNECTIVITY_H
