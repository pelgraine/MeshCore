#include "BluettiProtocol.h"
#include <string.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/bignum.h>

// Constants from bluetti-bt-lib 0.1.8 bluetooth/encryption.py
static const uint8_t LOCAL_AES_KEY[16] = {
  0x45, 0x9F, 0xC5, 0x35, 0x80, 0x89, 0x41, 0xF1, 0x70, 0x91, 0xE0, 0x99, 0x3E, 0xE3, 0xE9, 0x3D
};
// PRIVATE_KEY_L1: signs our public key
static const uint8_t PRIVATE_KEY_L1[32] = {
  0x4F, 0x19, 0xA1, 0x6E, 0x3E, 0x87, 0xBD, 0xD9, 0xBD, 0x24, 0xD3, 0xE5, 0x49, 0x5B, 0x88, 0x04,
  0x15, 0x11, 0x94, 0x3C, 0xBC, 0x8B, 0x96, 0x9A, 0xDE, 0x96, 0x41, 0xD0, 0xF5, 0x6A, 0xF3, 0x37
};
// PUBLIC_KEY_K2 (point after the DER prefix): checks the unit's signature
static const uint8_t PUBLIC_KEY_K2[64] = {
  0xA7, 0x3A, 0xBF, 0x5D, 0x22, 0x32, 0xC8, 0xC1, 0xC7, 0x2E, 0x68, 0x30, 0x43, 0x43, 0xC2, 0x72,
  0x49, 0x5E, 0x3A, 0x8F, 0xD6, 0xF3, 0x0E, 0xA9, 0x6D, 0xE2, 0xF4, 0xB3, 0xCE, 0x60, 0xB2, 0x51,
  0xEE, 0x21, 0xAC, 0x66, 0x7C, 0xF8, 0xA7, 0x1E, 0x18, 0xB4, 0x6B, 0x66, 0x4E, 0xAE, 0xFF, 0xE3,
  0xC4, 0x89, 0xF2, 0x4F, 0x69, 0x5B, 0x64, 0x11, 0xDB, 0x7E, 0x22, 0xCC, 0xC8, 0x5A, 0x85, 0x94
};
static const uint8_t* s_unit_key = PUBLIC_KEY_K2;
static uint8_t s_test_key[64];

#define KEX_MAGIC0  0x2A    // "**"
#define KEX_MAGIC1  0x2A

#define MSG_CHALLENGE           1
#define MSG_CHALLENGE_ACCEPTED  3
#define MSG_PEER_PUBKEY         4
#define MSG_PUBKEY_ACCEPTED     6

#define AES_BLOCK  16

/* ------------------------------- MODBUS ------------------------------------- */

uint16_t bluettiModbusCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
  }
  return crc;
}

size_t bluettiBuildRead(uint8_t* out, uint16_t addr, uint16_t qty) {
  out[0] = 0x01;
  out[1] = 0x03;
  out[2] = addr >> 8;
  out[3] = addr & 0xFF;
  out[4] = qty >> 8;
  out[5] = qty & 0xFF;
  uint16_t crc = bluettiModbusCrc(out, 6);
  out[6] = crc & 0xFF;   // little endian, as the library packs it
  out[7] = crc >> 8;
  return 8;
}

bool bluettiParseRead(const uint8_t* resp, size_t len, uint16_t qty, const uint8_t*& body) {
  size_t want = 5 + 2 * (size_t)qty;
  if (len != want) return false;
  if (resp[1] != 0x03 || resp[2] != 2 * qty) return false;
  uint16_t crc = bluettiModbusCrc(resp, len - 2);
  if (resp[len - 2] != (crc & 0xFF) || resp[len - 1] != (crc >> 8)) return false;
  body = resp + 3;
  return true;
}

/* ------------------------------- helpers ------------------------------------ */

// 2 byte big endian sum of the body ("hexsum" in the library)
static uint16_t bodySum(const uint8_t* body, size_t len) {
  uint32_t s = 0;
  for (size_t i = 0; i < len; i++) s += body[i];
  return (uint16_t)s;
}

static void md5(const uint8_t* in, size_t len, uint8_t out[16]) {
  mbedtls_md5_ret(in, len, out);
}

static bool aesCbc(int mode, const uint8_t* key, size_t key_len, const uint8_t iv_in[16],
                   const uint8_t* in, size_t len, uint8_t* out) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = (mode == MBEDTLS_AES_ENCRYPT) ? mbedtls_aes_setkey_enc(&aes, key, key_len * 8)
                                         : mbedtls_aes_setkey_dec(&aes, key, key_len * 8);
  uint8_t iv[16];
  memcpy(iv, iv_in, 16);   // mbedTLS updates the IV in place
  if (rc == 0) rc = mbedtls_aes_crypt_cbc(&aes, mode, len, iv, in, out);
  mbedtls_aes_free(&aes);
  return rc == 0;
}

// Encrypted frame: <plain length, 2 bytes big endian> [<4 byte IV seed>] <AES-CBC data, zero padded>
// The IV seed is only present when 'iv' is NULL (IV = MD5 of the seed).
static size_t aesEncryptFrame(const uint8_t* data, size_t len, const uint8_t* key, size_t key_len,
                              const uint8_t* iv, uint8_t* out, size_t out_max) {
  size_t padded = ((len + AES_BLOCK - 1) / AES_BLOCK) * AES_BLOCK;
  size_t header = iv ? 2 : 6;
  if (header + padded > out_max || padded > BLUETTI_MAX_MSG) return 0;

  out[0] = len >> 8;
  out[1] = len & 0xFF;
  uint8_t iv_buf[16];
  if (iv == NULL) {
    bluettiRandom(NULL, &out[2], 4);
    md5(&out[2], 4, iv_buf);
    iv = iv_buf;
  }
  uint8_t plain[BLUETTI_MAX_MSG];
  memset(plain, 0, padded);
  memcpy(plain, data, len);
  if (!aesCbc(MBEDTLS_AES_ENCRYPT, key, key_len, iv, plain, padded, &out[header])) return 0;
  return header + padded;
}

/* ------------------------------- session ------------------------------------ */

struct EcState {
  mbedtls_ecp_group  grp;
  mbedtls_mpi        d;     // our private key
  mbedtls_ecp_point  Q;     // our public key
  mbedtls_ecp_point  Qp;    // the unit's public key
};

static void ecInit(EcState* s) {
  mbedtls_ecp_group_init(&s->grp);
  mbedtls_mpi_init(&s->d);
  mbedtls_ecp_point_init(&s->Q);
  mbedtls_ecp_point_init(&s->Qp);
  mbedtls_ecp_group_load(&s->grp, MBEDTLS_ECP_DP_SECP256R1);
}

static void ecFree(EcState* s) {
  mbedtls_ecp_group_free(&s->grp);
  mbedtls_mpi_free(&s->d);
  mbedtls_ecp_point_free(&s->Q);
  mbedtls_ecp_point_free(&s->Qp);
}

// 64 byte X||Y to a point on the curve
static bool readPoint(EcState* s, mbedtls_ecp_point* P, const uint8_t* xy) {
  uint8_t buf[65];
  buf[0] = 0x04;
  memcpy(&buf[1], xy, 64);
  if (mbedtls_ecp_point_read_binary(&s->grp, P, buf, 65) != 0) return false;
  return mbedtls_ecp_check_pubkey(&s->grp, P) == 0;
}

void BluettiSession::setUnitSigningKeyForTest(const uint8_t* xy64) {
  if (xy64) {
    memcpy(s_test_key, xy64, 64);
    s_unit_key = s_test_key;
  } else {
    s_unit_key = PUBLIC_KEY_K2;
  }
}

BluettiSession::BluettiSession() {
  _ecdh = new EcState;
  ecInit((EcState*)_ecdh);
  reset();
}

BluettiSession::~BluettiSession() {
  ecFree((EcState*)_ecdh);
  delete (EcState*)_ecdh;
}

void BluettiSession::reset() {
  _have_temp = false;
  _ready = false;
  _have_peer = false;
  _buf_len = 0;
  _resp_len = 0;
  _err = "";
  memset(_temp_key, 0, sizeof(_temp_key));
  memset(_temp_iv, 0, sizeof(_temp_iv));
  memset(_secure_key, 0, sizeof(_secure_key));
}

size_t BluettiSession::expectedCipherLen() const {
  if (_buf_len < 2) return 0;
  size_t data_len = ((size_t)_buf[0] << 8) | _buf[1];
  size_t header = _ready ? 6 : 2;    // the IV seed is only sent once the secure key is in use
  size_t padded = ((data_len + AES_BLOCK - 1) / AES_BLOCK) * AES_BLOCK;
  return header + padded;
}

bool BluettiSession::onNotify(const uint8_t* data, size_t len, const Writer& writer) {
  // handshake messages before encryption start with "**"
  if (_buf_len == 0 && len >= 2 && data[0] == KEX_MAGIC0 && data[1] == KEX_MAGIC1) {
    return handlePlain(data, len, writer);
  }
  if (!_have_temp) return fail("encrypted data before the challenge");

  if (_buf_len + len > sizeof(_buf)) {
    _buf_len = 0;
    return fail("encrypted message too long");
  }
  memcpy(&_buf[_buf_len], data, len);
  _buf_len += len;

  for (;;) {
    size_t want = expectedCipherLen();
    if (want == 0 || _buf_len < want) return true;   // wait for more fragments
    if (want > sizeof(_buf)) {
      _buf_len = 0;
      return fail("encrypted message too long");
    }

    size_t data_len = ((size_t)_buf[0] << 8) | _buf[1];
    uint8_t plain[BLUETTI_MAX_MSG];
    bool ok;
    if (_ready) {
      uint8_t iv[16];
      md5(&_buf[2], 4, iv);
      ok = aesCbc(MBEDTLS_AES_DECRYPT, _secure_key, 32, iv, &_buf[6], want - 6, plain);
    } else {
      ok = aesCbc(MBEDTLS_AES_DECRYPT, _temp_key, 16, _temp_iv, &_buf[2], want - 2, plain);
    }

    // keep anything after this message for the next one
    memmove(_buf, &_buf[want], _buf_len - want);
    _buf_len -= want;

    if (!ok) {
      _buf_len = 0;
      return fail("decryption failed");
    }
    if (!handleDecrypted(plain, data_len, writer)) return false;
  }
}

bool BluettiSession::handlePlain(const uint8_t* msg, size_t len, const Writer& writer) {
  // "**" <type> <length> <data> <2 byte sum of type..data>
  if (len < 6) return true;   // too short to be anything; ignored, as the library does
  const uint8_t* body = msg + 2;
  uint8_t type = body[0];
  const uint8_t* d = body + 2;
  size_t d_len = len - 6;

  if (type == MSG_CHALLENGE) {
    if (d_len != 4) return fail("challenge has the wrong length");
    uint8_t rev[4] = { d[3], d[2], d[1], d[0] };
    md5(rev, 4, _temp_iv);
    for (int i = 0; i < 16; i++) _temp_key[i] = _temp_iv[i] ^ LOCAL_AES_KEY[i];
    _have_temp = true;

    uint8_t reply[2 + 6 + 2];
    reply[0] = KEX_MAGIC0;
    reply[1] = KEX_MAGIC1;
    reply[2] = 0x02;
    reply[3] = 0x04;
    memcpy(&reply[4], &_temp_iv[8], 4);
    uint16_t sum = bodySum(&reply[2], 6);
    reply[8] = sum >> 8;
    reply[9] = sum & 0xFF;
    if (!writer(reply, sizeof(reply))) return fail("writing the challenge reply failed");
    return true;
  }
  // MSG_CHALLENGE_ACCEPTED and anything else: nothing to do
  return true;
}

bool BluettiSession::handleDecrypted(const uint8_t* msg, size_t len, const Writer& writer) {
  if (len >= 6 && msg[0] == KEX_MAGIC0 && msg[1] == KEX_MAGIC1) {
    uint8_t type = msg[2];
    const uint8_t* d = msg + 4;
    size_t d_len = len - 6;
    if (type == MSG_PEER_PUBKEY) return handlePeerPubkey(d, d_len, writer);
    if (type == MSG_PUBKEY_ACCEPTED) return handleKeyAccepted(d, d_len);
  }
  if (len > sizeof(_resp)) return fail("reply too long");
  memcpy(_resp, msg, len);
  _resp_len = len;
  return true;
}

bool BluettiSession::handlePeerPubkey(const uint8_t* d, size_t d_len, const Writer& writer) {
  if (d_len != 128) return fail("unit public key message has the wrong length");
  EcState* s = (EcState*)_ecdh;

  // the unit signs <its public key><temporary IV>
  uint8_t signed_data[64 + 16];
  memcpy(signed_data, d, 64);
  memcpy(&signed_data[64], _temp_iv, 16);
  uint8_t hash[32];
  mbedtls_sha256_ret(signed_data, sizeof(signed_data), hash, 0);

  bool ok = false;
  mbedtls_ecp_point K2;
  mbedtls_mpi r, sv;
  mbedtls_ecp_point_init(&K2);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&sv);
  do {
    if (!readPoint(s, &K2, s_unit_key)) { fail("bad signing key"); break; }
    if (mbedtls_mpi_read_binary(&r, &d[64], 32) != 0) { fail("bad signature"); break; }
    if (mbedtls_mpi_read_binary(&sv, &d[96], 32) != 0) { fail("bad signature"); break; }
    if (mbedtls_ecdsa_verify(&s->grp, hash, 32, &K2, &r, &sv) != 0) { fail("unit signature check failed"); break; }
    if (!readPoint(s, &s->Qp, d)) { fail("unit public key is not valid"); break; }
    ok = true;
  } while (0);
  mbedtls_ecp_point_free(&K2);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&sv);
  if (!ok) return false;
  _have_peer = true;

  // our key pair for this connection
  if (mbedtls_ecp_gen_keypair(&s->grp, &s->d, &s->Q, bluettiRandom, NULL) != 0) return fail("key generation failed");
  uint8_t pub65[65];
  size_t olen = 0;
  if (mbedtls_ecp_point_write_binary(&s->grp, &s->Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub65, sizeof(pub65)) != 0 || olen != 65) {
    return fail("public key export failed");
  }

  // sign <our public key><temporary IV> with the well known key
  uint8_t to_sign[64 + 16];
  memcpy(to_sign, &pub65[1], 64);
  memcpy(&to_sign[64], _temp_iv, 16);
  mbedtls_sha256_ret(to_sign, sizeof(to_sign), hash, 0);

  // "**" 05 80 <our public key, 64> <signature r||s, 64> <sum>
  uint8_t msg[2 + 2 + 128 + 2];
  msg[0] = KEX_MAGIC0;
  msg[1] = KEX_MAGIC1;
  msg[2] = 0x05;
  msg[3] = 0x80;
  memcpy(&msg[4], &pub65[1], 64);

  mbedtls_mpi l1, sr, ss;
  mbedtls_mpi_init(&l1);
  mbedtls_mpi_init(&sr);
  mbedtls_mpi_init(&ss);
  ok = mbedtls_mpi_read_binary(&l1, PRIVATE_KEY_L1, 32) == 0
    && mbedtls_ecdsa_sign(&s->grp, &sr, &ss, &l1, hash, 32, bluettiRandom, NULL) == 0
    && mbedtls_mpi_write_binary(&sr, &msg[68], 32) == 0
    && mbedtls_mpi_write_binary(&ss, &msg[100], 32) == 0;
  mbedtls_mpi_free(&l1);
  mbedtls_mpi_free(&sr);
  mbedtls_mpi_free(&ss);
  if (!ok) return fail("signing failed");

  uint16_t sum = bodySum(&msg[2], 2 + 128);
  msg[132] = sum >> 8;
  msg[133] = sum & 0xFF;

  uint8_t out[BLUETTI_MAX_CIPHER];
  size_t out_len = aesEncryptFrame(msg, sizeof(msg), _temp_key, 16, _temp_iv, out, sizeof(out));
  if (out_len == 0) return fail("encrypting our public key failed");
  if (!writer(out, out_len)) return fail("writing our public key failed");
  return true;
}

bool BluettiSession::handleKeyAccepted(const uint8_t* d, size_t d_len) {
  if (d_len != 1) return fail("key accepted message has the wrong length");
  if (d[0] != 0) return fail("unit did not accept our key");
  if (!_have_peer) return fail("key accepted before the unit sent its key");
  EcState* s = (EcState*)_ecdh;

  mbedtls_mpi z;
  mbedtls_mpi_init(&z);
  bool ok = mbedtls_ecdh_compute_shared(&s->grp, &z, &s->Qp, &s->d, bluettiRandom, NULL) == 0
         && mbedtls_mpi_write_binary(&z, _secure_key, 32) == 0;
  mbedtls_mpi_free(&z);
  if (!ok) return fail("shared key calculation failed");
  _ready = true;
  return true;
}

size_t BluettiSession::encryptCommand(const uint8_t* cmd, size_t cmd_len, uint8_t* out, size_t out_max) {
  if (!_ready) return 0;
  return aesEncryptFrame(cmd, cmd_len, _secure_key, 32, NULL, out, out_max);
}
