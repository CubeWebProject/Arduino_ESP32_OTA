/*
   This file is part of Arduino_ESP32_OTA.

   Copyright 2022 ARDUINO SA (http://www.arduino.cc/)

   This software is released under the GNU General Public License version 3,
   which covers the main part of arduino-cli.
   The terms of this license can be found at:
   https://www.gnu.org/licenses/gpl-3.0.en.html

   You can be released from the requirements of the above licenses by purchasing
   a commercial license. Buying such a license is mandatory if you want to modify or
   otherwise use the software for commercial activities involving the Arduino
   software without disclosing the source code of your own applications. To purchase
   a commercial license, send an email to license@arduino.cc.
*/

/******************************************************************************
   INCLUDE
 ******************************************************************************/

#include <Update.h>
#include "Arduino_ESP32_OTA.h"
#include "decompress/lzss.h"
#include "decompress/utility.h"
#include "esp_ota_ops.h"

/* Used to bind local module function to actual class instance */
static Arduino_ESP32_OTA * _esp_ota_obj_ptr = 0;

/******************************************************************************
   LOCAL MODULE FUNCTIONS
 ******************************************************************************/

static uint8_t read_byte() {
  if(_esp_ota_obj_ptr) {
    return _esp_ota_obj_ptr->read_byte_from_network();
  }
  return -1;
}

static void write_byte(uint8_t data) {
  if(_esp_ota_obj_ptr) {
    _esp_ota_obj_ptr->write_byte_to_flash(data);
  }
}

/******************************************************************************
   CTOR/DTOR
 ******************************************************************************/

Arduino_ESP32_OTA::Arduino_ESP32_OTA()
:_client{nullptr}
,_ota_header{0}
,_ota_size(0)
,_crc32(0)
{

}

/******************************************************************************
   PUBLIC MEMBER FUNCTIONS
 ******************************************************************************/

Arduino_ESP32_OTA::Error Arduino_ESP32_OTA::begin()
{
  _esp_ota_obj_ptr = this;

  /* ... initialize CRC ... */
  _crc32 = 0xFFFFFFFF;
  
  if(!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    DEBUG_ERROR("%s: failed to initialize flash update", __FUNCTION__);
    return Error::OtaStorageInit;
  }
  return Error::None;
}


uint8_t Arduino_ESP32_OTA::read_byte_from_network()
{
  bool is_http_data_timeout = false;
  for(unsigned long const start = millis();;)
  {
    is_http_data_timeout = (millis() - start) > ARDUINO_ESP32_OTA_BINARY_BYTE_RECEIVE_TIMEOUT_ms;
    if (is_http_data_timeout) {
      DEBUG_ERROR("%s: timeout waiting data", __FUNCTION__);
      return -1;
    }
    if (_client->available()) {
      const uint8_t data = _client->read();
      _crc32 = crc_update(_crc32, &data, 1);
      return data;
    }
  }
}

void Arduino_ESP32_OTA::write_byte_to_flash(uint8_t data)
{
  Update.write(&data, 1);
}

int Arduino_ESP32_OTA::download(WiFiClient* client)
{

    _client = client;
 

  

  _client->println("ota");

  /* Receive HTTP header. */
  String http_header;
  bool is_header_complete     = false,
       is_http_header_timeout = false;
  for (unsigned long const start = millis(); !is_header_complete;)
  {
    is_http_header_timeout = (millis() - start) > ARDUINO_ESP32_OTA_HTTP_HEADER_RECEIVE_TIMEOUT_ms;
    if (is_http_header_timeout) break;

    if (_client->available())
    {
      char const c = _client->read();

      http_header += c;
      if (http_header.endsWith("\r\n\r\n"))
        is_header_complete = true;
    }
  }

  if (!is_header_complete)
  {
    DEBUG_ERROR("%s: Error receiving HTTP header %s", __FUNCTION__, is_http_header_timeout ? "(timeout)":"");
    return static_cast<int>(Error::HttpHeaderError);
  }

  /* TODO check http header 200 or else*/

  /* Extract concent length from HTTP header. A typical entry looks like
   *   "Content-Length: 123456"
   */
  char const * content_length_ptr = strstr(http_header.c_str(), "Content-Length");
  if (!content_length_ptr)
  {
    DEBUG_ERROR("%s: Failure to extract content length from http header", __FUNCTION__);
    return static_cast<int>(Error::ParseHttpHeader);
  }
  /* Find start of numerical value. */
  char * ptr = const_cast<char *>(content_length_ptr);
  for (; (*ptr != '\0') && !isDigit(*ptr); ptr++) { }
  /* Extract numerical value. */
  String content_length_str;
  for (; isDigit(*ptr); ptr++) content_length_str += *ptr;
  int const content_length_val = atoi(content_length_str.c_str());
  DEBUG_VERBOSE("%s: Length of OTA binary according to HTTP header = %d bytes", __FUNCTION__, content_length_val);

  /* Read the OTA header ... */
  bool is_ota_header_timeout  = false;
  unsigned long const start = millis();
  for (int i = 0; i < sizeof(OtaHeader);)
  {
    is_ota_header_timeout = (millis() - start) > ARDUINO_ESP32_OTA_BINARY_HEADER_RECEIVE_TIMEOUT_ms;
    if (is_ota_header_timeout) break;

    if (_client->available())
    {
      _ota_header.buf[i++] = _client->read();
    }
  }

  /* ... check for header download timeout ... */
  if (is_ota_header_timeout) {
    return static_cast<int>(Error::OtaHeaderTimeout);
  }

  /* ... then check if OTA header length field matches HTTP content length... */
  if (_ota_header.header.len != (content_length_val - sizeof(_ota_header.header.len) - sizeof(_ota_header.header.crc32))) {
    return static_cast<int>(Error::OtaHeaderLength);
  }

  /* ... and OTA magic number */
  if (_ota_header.header.magic_number != ARDUINO_ESP32_OTA_MAGIC)
  {
    return static_cast<int>(Error::OtaHeaterMagicNumber);
  }

  /* ... start CRC32 from OTA MAGIC ... */
  _crc32 = crc_update(_crc32, &_ota_header.header.magic_number, 12);

  /* Download and decode OTA file */
  _ota_size = lzss_download(read_byte, write_byte, content_length_val - sizeof(_ota_header));

  if(_ota_size <= content_length_val - sizeof(_ota_header))
  {
    return static_cast<int>(Error::OtaDownload);
  }

  return _ota_size;
}

Arduino_ESP32_OTA::Error Arduino_ESP32_OTA::update()
{
  /* ... then finalise ... */
  _crc32 ^= 0xFFFFFFFF;

  if(_crc32 != _ota_header.header.crc32) {
    DEBUG_ERROR("%s: CRC32 mismatch", __FUNCTION__);
    return Error::OtaHeaderCrc;
  }

  if (!Update.end(true)) {
    DEBUG_ERROR("%s: Failure to apply OTA update", __FUNCTION__);
    return Error::OtaStorageEnd;
  }

  return Error::None;
}

void Arduino_ESP32_OTA::reset()
{
  ESP.restart();
}
