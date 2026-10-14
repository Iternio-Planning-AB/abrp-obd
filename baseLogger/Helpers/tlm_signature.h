#pragma once
/*
  Request signature for the bulk upload (RFC 9421 HTTP Message Signatures, RFC 9530 Content-Digest).
  Contains: tlmSignatureHeaders(), a pure builder - no globals, no I/O.

  Why: the modem verifies the server's chain but not its name (DONGLE_CONTEXT.md §6a), so anyone who
  can redirect the traffic could read a token sent in a header. Signed, the token never leaves the
  dongle over cellular; the server finds it by keyid and checks the HMAC. One fixed profile, shared with
  the backend's verifier:
    Content-Digest:  sha-256=:<b64 sha256(body as sent - the gzip bytes when compressed)>:
    Signature-Input: s=("content-digest");keyid="<first 32 hex of sha256(token)>"
    Signature:       s=:<b64 HMAC-SHA256(key = token bytes, RFC 9421 §2.5 signature base)>:
  The base is two lines joined by \n, no trailing newline:
    "content-digest": <the Content-Digest value>
    "@signature-params": <the Signature-Input value without "s=">
  Only the body is covered: the server accepts this signature on POST /2/app/tlm alone, and the token
  signs nothing else, so @method and @path would add bytes without adding protection. No alg (the
  verifier fixes HMAC-SHA256) and no created, so a captured request verifies again whenever it is
  replayed; only the server's handling of duplicate points limits that. created would fit the budget
  (the User-Agent would yield) but needs the verifier to check it, so it is a change on both sides.

  This file is #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit.
*/

#include <mbedtls/md.h>
#include <mbedtls/base64.h>

struct TlmSignatureHeaders {
  std::string content_digest, signature_input, signature;
};

static bool tlmBase64(const unsigned char* data, size_t len, std::string& out) {
  unsigned char text[48]; // 32 bytes -> 44 characters + NUL
  size_t n = 0;
  if (mbedtls_base64_encode(text, sizeof(text), &n, data, len) != 0) return false;
  out.assign((const char*)text, n);
  return true;
}

// wire_sha256: SHA-256 of the request body exactly as it goes on the wire. Returns false - and nothing
// must be sent - if any step fails.
bool tlmSignatureHeaders(const std::string& token, const unsigned char wire_sha256[32], TlmSignatureHeaders& out) {
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  unsigned char hash[32];
  std::string b64;
  if (token.empty() || !sha256 || !tlmBase64(wire_sha256, 32, b64)) return false;
  out.content_digest = "sha-256=:" + b64 + ":";

  // Recomputed every time rather than cached: the token can change with a profile push.
  if (mbedtls_md(sha256, (const unsigned char*)token.data(), token.size(), hash) != 0) return false;
  char keyid[33];
  for (int i = 0; i < 16; ++i) snprintf(keyid + i * 2, 3, "%02x", hash[i]);
  const std::string params = std::string("(\"content-digest\");keyid=\"") + keyid + "\"";
  out.signature_input = "s=" + params;

  const std::string base = "\"content-digest\": " + out.content_digest + "\n\"@signature-params\": " + params;
  if (mbedtls_md_hmac(sha256, (const unsigned char*)token.data(), token.size(),
        (const unsigned char*)base.data(), base.size(), hash) != 0 || !tlmBase64(hash, 32, b64)) return false;
  out.signature = "s=:" + b64 + ":";
  return true;
}
