#pragma once

// Bluetti "IoT protocol version 2" over Bluetooth LE, with encryption, as implemented by
// bluetti-bt-lib 0.1.8 (bluetooth/device_reader.py, bluetooth/encryption.py, registers/).
//
// Plain MODBUS style register reads, wrapped in AES-CBC once a key exchange is done:
//   1. unit sends a challenge (plain, "**" magic); we answer it, giving a temporary AES key/IV
//   2. unit sends its public key, signed (encrypted with the temporary key)
//   3. we send our own public key, signed with the well known key (encrypted with the temporary key)
//   4. unit accepts; the ECDH shared secret is the AES-256 key for the rest of the connection
//
// No Bluetooth code in here, so it can be tested on a PC.

#include <stdint.h>
#include <stddef.h>
#include <functional>

#define BLUETTI_MAX_MSG       256    // largest decrypted message we handle
#define BLUETTI_MAX_CIPHER    (6 + BLUETTI_MAX_MSG)

// Random bytes, for mbedTLS (returns 0 on success). Defined per platform.
int bluettiRandom(void* ctx, unsigned char* out, size_t len);

uint16_t bluettiModbusCrc(const uint8_t* data, size_t len);

// MODBUS "read holding registers" request: 01 03 <addr hi lo> <qty hi lo> <crc lo hi>. Returns 8.
size_t bluettiBuildRead(uint8_t* out, uint16_t addr, uint16_t qty);

// Checks a read reply (01 03 <byte count> <data> <crc lo hi>) for 'qty' registers.
// On success points 'body' at the register data (2 bytes per register, big endian).
bool bluettiParseRead(const uint8_t* resp, size_t len, uint16_t qty, const uint8_t*& body);

class BluettiSession {
public:
  typedef std::function<bool(const uint8_t* data, size_t len)> Writer;

  BluettiSession();
  ~BluettiSession();

  // Call at the start of every connection
  void reset();

  // Feed one Bluetooth notification. Handshake replies are sent through 'writer'.
  // Returns false on a protocol error (the connection should be dropped).
  bool onNotify(const uint8_t* data, size_t len, const Writer& writer);

  // Key exchange finished; commands can be sent
  bool isReady() const { return _ready; }

  // Encrypts a command for sending once ready. Returns 0 on failure.
  size_t encryptCommand(const uint8_t* cmd, size_t cmd_len, uint8_t* out, size_t out_max);

  // Latest decrypted reply (not part of the handshake)
  bool hasResponse() const { return _resp_len > 0; }
  const uint8_t* response() const { return _resp; }
  size_t responseLen() const { return _resp_len; }
  void clearResponse() { _resp_len = 0; }

  // Short description of the last failure, for diagnostics
  const char* lastError() const { return _err; }

  // For tests only: replaces the public key the unit's signature is checked against
  // (64 bytes, X then Y). NULL restores the built in key.
  static void setUnitSigningKeyForTest(const uint8_t* xy64);

private:
  bool     _have_temp;
  uint8_t  _temp_key[16];
  uint8_t  _temp_iv[16];
  bool     _ready;
  uint8_t  _secure_key[32];
  void*    _ecdh;           // our key pair and the unit's public key (mbedTLS objects)
  bool     _have_peer;

  uint8_t  _buf[BLUETTI_MAX_CIPHER];
  size_t   _buf_len;
  uint8_t  _resp[BLUETTI_MAX_MSG];
  size_t   _resp_len;
  const char* _err;

  bool handlePlain(const uint8_t* msg, size_t len, const Writer& writer);
  bool handleDecrypted(const uint8_t* msg, size_t len, const Writer& writer);
  bool handlePeerPubkey(const uint8_t* data, size_t len, const Writer& writer);
  bool handleKeyAccepted(const uint8_t* data, size_t len);
  size_t expectedCipherLen() const;
  bool fail(const char* why) { _err = why; return false; }
};
