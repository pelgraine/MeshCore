#pragma once

#include <Arduino.h>
#include <Packet.h>
#include <SHA256.h>
#include <helpers/TransportKeyStore.h>

// Channel the bot answers on. Must match the channel name exactly.
#ifndef MAGIC8BALL_CHANNEL_NAME
  #define MAGIC8BALL_CHANNEL_NAME  "#magic8ball"
#endif

// Region scope an incoming message must carry to get an answer.
#ifndef MAGIC8BALL_SCOPE_NAME
  #define MAGIC8BALL_SCOPE_NAME  "#au-nsw-syd"
#endif

#define MAGIC8BALL_NUM_ANSWERS  20

// Per-sender rate limit: more than MAGIC8BALL_MAX_IN_WINDOW queries inside
// MAGIC8BALL_WINDOW_MS puts that sender into a MAGIC8BALL_PENALTY_MS timeout,
// during which the bot does not answer them.
#ifndef MAGIC8BALL_MAX_SENDERS
  #define MAGIC8BALL_MAX_SENDERS    128
#endif
#define MAGIC8BALL_MAX_IN_WINDOW    3
#define MAGIC8BALL_WINDOW_MS        15000
#define MAGIC8BALL_PENALTY_MS       30000


// The standard 20 Magic 8-Ball responses: 10 affirmative, 5 non-committal, 5 negative.
static const char* MAGIC8BALL_ANSWERS[MAGIC8BALL_NUM_ANSWERS] = {
  "It is certain",
  "It is decidedly so",
  "Without a doubt",
  "Yes definitely",
  "You may rely on it",
  "As I see it, yes",
  "Most likely",
  "Outlook good",
  "Yes",
  "Signs point to yes",
  "Reply hazy, try again",
  "Ask again later",
  "Better not tell you now",
  "Cannot predict now",
  "Concentrate and ask again",
  "Don't count on it",
  "My reply is no",
  "My sources say no",
  "Outlook not so good",
  "Very doubtful"
};

// True if 'text' ends with a question mark, ignoring trailing whitespace.
static bool magic8ball_isQuery(const char* text) {
  if (text == NULL) return false;
  int i = strlen(text);
  while (i > 0 && (text[i-1] == ' ' || text[i-1] == '\t' || text[i-1] == '\r' || text[i-1] == '\n')) {
    i--;
  }
  return i > 0 && text[i-1] == '?';
}

// True if the packet carries transport codes matching 'scope_name'.
// Key derivation matches RegionMap::getTransportKeysFor(): SHA-256 over the
// region name with a leading '#', and the code is compared against slot 0,
// which is the scope the sender transmitted under.
static bool magic8ball_scopeMatches(const mesh::Packet* pkt, const char* scope_name) {
  if (pkt == NULL || !pkt->hasTransportCodes()) return false;

  char tmp[32];
  if (scope_name[0] == '#') {
    strncpy(tmp, scope_name, sizeof(tmp) - 1);
  } else {
    tmp[0] = '#';
    strncpy(&tmp[1], scope_name, sizeof(tmp) - 2);
  }
  tmp[sizeof(tmp) - 1] = 0;

  TransportKey key;
  SHA256 sha;
  sha.update(tmp, strlen(tmp));
  sha.finalize(key.key, sizeof(key.key));

  return pkt->transport_codes[0] == key.calcTransportCode(pkt);
}

// ---------------------------------------------------------------------------
// Per-sender rate limiting
//
// Channel messages are unverified, so the only sender identity available is
// the "<name>: " prefix that sendGroupMessage() writes into the payload. It is
// a display name, not a key, and can be spoofed or duplicated.
// ---------------------------------------------------------------------------

struct Magic8BallSender {
  char     name[32];
  uint32_t hits[MAGIC8BALL_MAX_IN_WINDOW];
  uint8_t  num_hits;
  bool     in_penalty;
  uint32_t penalty_until;
  uint32_t last_seen;
};

static Magic8BallSender m8_senders[MAGIC8BALL_MAX_SENDERS];
static int m8_num_senders = 0;

// Copies the "<name>: " prefix of 'text' into dest. Returns false if absent.
static bool magic8ball_senderName(const char* text, char* dest, size_t dest_size) {
  if (text == NULL) return false;
  const char* sep = strchr(text, ':');
  if (sep == NULL || sep == text) return false;

  size_t n = sep - text;
  if (n > dest_size - 1) n = dest_size - 1;
  memcpy(dest, text, n);
  dest[n] = 0;
  return true;
}

// True if this sender is allowed an answer right now. Records the query when
// it is allowed. 'now' must be a monotonic millisecond count.
static bool magic8ball_allow(const char* text, uint32_t now) {
  char name[32];
  if (!magic8ball_senderName(text, name, sizeof(name))) return false;

  Magic8BallSender* s = NULL;
  for (int i = 0; i < m8_num_senders; i++) {
    if (strcmp(m8_senders[i].name, name) == 0) {
      s = &m8_senders[i];
      break;
    }
  }

  if (s == NULL) {
    if (m8_num_senders < MAGIC8BALL_MAX_SENDERS) {
      s = &m8_senders[m8_num_senders++];
    } else {
      // table full: reuse the least recently seen entry
      s = &m8_senders[0];
      for (int i = 1; i < m8_num_senders; i++) {
        if ((int32_t)(m8_senders[i].last_seen - s->last_seen) < 0) s = &m8_senders[i];
      }
    }
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
  }
  s->last_seen = now;

  if (s->in_penalty) {
    if ((int32_t)(now - s->penalty_until) < 0) return false;  // still serving the timeout
    s->in_penalty = false;
    s->num_hits = 0;
  }

  // drop hits that have aged out of the window
  uint8_t kept = 0;
  for (uint8_t i = 0; i < s->num_hits; i++) {
    if ((int32_t)(now - s->hits[i]) < MAGIC8BALL_WINDOW_MS) {
      s->hits[kept++] = s->hits[i];
    }
  }
  s->num_hits = kept;

  if (s->num_hits >= MAGIC8BALL_MAX_IN_WINDOW) {
    s->in_penalty = true;
    s->penalty_until = now + MAGIC8BALL_PENALTY_MS;
    s->num_hits = 0;
    return false;
  }

  s->hits[s->num_hits++] = now;
  return true;
}

