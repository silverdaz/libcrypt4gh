#include <unistd.h>
#include <sys/types.h>
#include <sodium.h>
#include <errno.h>

#include "includes.h"

#include "packet.h"
#include "header.h"

static char* MAGIC_NUMBER = "crypt4gh";
static uint32_t VERSION = 1U;


static int
header_encrypt_X25519_Chacha20_Poly1305(const uint8_t* data, size_t data_len,
					const uint8_t pubkey[crypto_kx_PUBLICKEYBYTES],
					const uint8_t seckey[crypto_kx_SECRETKEYBYTES],
					const uint8_t recipient_pubkey[crypto_kx_PUBLICKEYBYTES],
					uint8_t output[CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len])
{
  int rc = 0;


  /* X25519 shared key */
  uint8_t* shared_key = (uint8_t*)sodium_malloc(crypto_kx_SESSIONKEYBYTES);
  if(!shared_key || errno == ENOMEM){
    D1("Unable to allocated memory for the shared key");
    return 1;
  }

  H1("-----    seckey", seckey, crypto_kx_SECRETKEYBYTES);
  H1("-----    pubkey", pubkey, crypto_kx_PUBLICKEYBYTES);
  H1("----- recipient", recipient_pubkey, crypto_kx_PUBLICKEYBYTES);
  
  uint8_t ignored[crypto_kx_SESSIONKEYBYTES];
  rc = crypto_kx_server_session_keys(ignored, shared_key, pubkey, seckey, recipient_pubkey);
  sodium_memzero(ignored, crypto_kx_SESSIONKEYBYTES);
  sodium_mprotect_readonly(shared_key);

  if(rc){
    D1("Unable to derive the shared key: %d", rc);
    goto bailout;
  }

  H1("Shared key", shared_key, crypto_kx_SESSIONKEYBYTES);

  /* Chacha20_Poly1305 */
  unsigned char nonce[CRYPT4GH_NONCE_SIZE];
  randombytes_buf(nonce, CRYPT4GH_NONCE_SIZE); /* CRYPT4GH_NONCE_SIZE * sizeof(char) */

  uint8_t* p = output; /* record the start */

  /* length */
  PUT_32BIT_LE(p, CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len);
  D1("Packet length: %d", CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len - 4);
  H1("Packet len", p, 4);
  p+=4;

  /* encryption method */
  PUT_32BIT_LE(p, X25519_chacha20_ietf_poly1305);
  D1("Encryption method: %d", X25519_chacha20_ietf_poly1305);
  H1("enc method", p, 4);
  p+=4;

  /* sender's pubkey */
  memcpy(p, pubkey, crypto_box_PUBLICKEYBYTES);
  H1("Sender's pubkey", p, crypto_box_PUBLICKEYBYTES);
  p+=crypto_box_PUBLICKEYBYTES;

  /* nonce */
  memcpy(p, nonce, CRYPT4GH_NONCE_SIZE);
  H1("nonce", p, CRYPT4GH_NONCE_SIZE);
  p+=CRYPT4GH_NONCE_SIZE;

  /* encrypted session key (and mac) */
  unsigned long long len;
  rc = crypto_aead_chacha20poly1305_ietf_encrypt(p, &len,
						 data, data_len,
						 NULL, 0, /* no authenticated data */
						 NULL, nonce, shared_key);
  if(rc){
    D1("Error %d encrypting the data", rc);
    goto bailout;
  }
  H1("Encrypted data", p, len);
  p+=len;
 
  if(p-output != CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len){
    D1("Error of size for the encrypted data packet");
    D2("packet: %p", output);
    D2("     p: %p", p);
    D2("diff: %ld", (p-output));
    D2("size: %d", CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len);
    rc = 4;
    goto bailout;
  }

  rc = 0; /* success */
 
bailout:
 sodium_free(shared_key);
 return rc;
}


int
crypt4gh_header_build(const uint8_t session_key[CRYPT4GH_SESSION_KEY_SIZE],
		      const uint8_t* seckey,
		      const uint8_t* recipient_pubkeys, unsigned int nrecipients,
		      uint8_t** output, size_t* output_len)
{
  if(recipient_pubkeys == NULL || nrecipients == 0){
    D1("No recipients");
    return 1;
  }

  /* Allocate space for n packets + magic_number and version */
  size_t buflen = (8+4+4+ nrecipients * CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len);

  uint8_t* buf = (uint8_t*)malloc(buflen);

  if(buf == NULL || errno == ENOMEM){
    D1("Could not allocate memory");
    return 2;
  }

  uint8_t* data_packet = NULL;
  size_t data_packet_len;
  int rc = crypt4gh_packet_build_data_enc(chacha20_ietf_poly1305, session_key, &data_packet, &data_packet_len);

  if(rc){
    D1("Error making data packet");
    goto bailout;
  }

  /* get public key from secret key */
  uint8_t pubkey[crypto_box_PUBLICKEYBYTES];
  rc = crypto_scalarmult_base(pubkey, seckey);
  if(rc){
    D1("Error retrieving the public key from the secret key");
    goto bailout;
  }
  H1("Public key", (uint8_t*)pubkey, 32);

  if(output) *output=buf;
  if(output_len) *output_len=buflen;

  /* Magic number */
  memcpy(buf, MAGIC_NUMBER, 8);
  D1("output magic number: %.8s", MAGIC_NUMBER);
  H1("Magic number", buf, 8);
  buf+=8;
  
  /* Version */
  PUT_32BIT_LE(buf, VERSION);
  D1("output version: %d", VERSION);
  H1("Version", buf, 4);
  buf+=4;

  /* Number of Packets */
  PUT_32BIT_LE(buf, nrecipients);
  D1("output nb packets: %d", nrecipients);
  H1("#packets", buf, 4);
  buf+=4;

  /* For each recipients */
  unsigned int i=0;
  for(; i<nrecipients; i++){  

    if(header_encrypt_X25519_Chacha20_Poly1305(data_packet, data_packet_len,
					       pubkey, seckey, recipient_pubkeys + (i * crypto_box_PUBLICKEYBYTES),
					       buf))
      { D1("Error encrypting for recipient %d", i);
	rc = i+1;
	goto bailout;
      }
    buf+=CRYPT4GH_HEADER_ENCRYPTED_DATA_PACKET_len;
  }
  
  if(output && (size_t)(buf - *output) != buflen){
    D1("Error of size for the encrypted data packets");
    D2("output: %p", *output);
    D2("buffer: %p", buf);
    D2("size: %zu", buflen);
    return i;
  }

bailout:
  if(data_packet) sodium_free(data_packet);
  if(rc){ /* error: cleanup */
    if(output) *output=NULL;
    if(output_len) *output_len=0;
    free(buf);
  }
  return rc;
}


static int
header_decrypt_X25519_Chacha20_Poly1305(const uint8_t seckey[crypto_box_SECRETKEYBYTES],
					const uint8_t pubkey[crypto_box_PUBLICKEYBYTES],
					uint8_t* data, unsigned int data_len,
					uint8_t* output, unsigned int* output_len)
{
  int rc = 0;

  if(output == NULL ||
     data_len < 4U + crypto_box_PUBLICKEYBYTES + CRYPT4GH_NONCE_SIZE + crypto_box_MACBYTES)
    {
      D1("Invalid input parameters");
      return 1;
    }

  /* encryption method */
  uint8_t* p = data;
  if(PEEK_U32_LE(p) != X25519_chacha20_ietf_poly1305)
    {
      E("Invalid encryption method");
      return 1;
    }
  p += 4;
  data_len -= 4;

  /* sender's pubkey */
  uint8_t sender_pubkey[crypto_box_PUBLICKEYBYTES];
  memcpy(sender_pubkey, p, crypto_box_PUBLICKEYBYTES);
  H1("Sender's pubkey", sender_pubkey, crypto_box_PUBLICKEYBYTES);
  p += crypto_box_PUBLICKEYBYTES;
  data_len -= crypto_box_PUBLICKEYBYTES;
  
  /* nonce */
  uint8_t nonce[CRYPT4GH_NONCE_SIZE];
  memcpy(nonce, p, CRYPT4GH_NONCE_SIZE);
  H1("nonce", p, CRYPT4GH_NONCE_SIZE);
  p += CRYPT4GH_NONCE_SIZE;
  data_len -= CRYPT4GH_NONCE_SIZE;

  /* X25519 shared key */
  uint8_t* shared_key = (uint8_t*)sodium_malloc(crypto_kx_SESSIONKEYBYTES);
  if(!shared_key || errno == ENOMEM){
    D1("Unable to allocated memory for the shared key");
    return 1;
  }
  
  uint8_t ignored[crypto_kx_SESSIONKEYBYTES];
  rc = crypto_kx_client_session_keys(shared_key, ignored, pubkey, seckey, sender_pubkey);
  sodium_memzero(ignored, crypto_kx_SESSIONKEYBYTES);
  sodium_mprotect_readonly(shared_key);

  if(rc){
    E("Unable to derive the shared key: %d", rc);
    goto bailout;
  }

  H1("Shared key", shared_key, crypto_kx_SESSIONKEYBYTES);

  /* decrypted packet (and mac) */
  D3("Encrypted Packet length %d", data_len);
  H3("Encrypted Data", p, data_len);
  unsigned long long decrypted_len;
  rc = crypto_aead_chacha20poly1305_ietf_decrypt(output, &decrypted_len,
						 NULL,
						 p, data_len,
						 NULL, 0, /* no authenticated data */
						 nonce, shared_key);
  if(rc){
    D1("Error decrypting the packet");
    goto bailout;
  }
  
  D3("Decrypted Packet length %llu", decrypted_len);
  if(output_len) *output_len = (unsigned int)decrypted_len; /* small enough, won't drop anything */

  rc = 0; /* success */

bailout:
  sodium_free(shared_key);
  return rc;
}

int
crypt4gh_header_parse(int fd,
		      const uint8_t seckey[crypto_box_SECRETKEYBYTES],
		      const uint8_t pubkey[crypto_box_PUBLICKEYBYTES],
		      uint8_t** session_keys, unsigned int* nkeys,
		      uint64_t** edit_list, unsigned int* edit_list_len)
{

  if(session_keys == NULL)
    {
      E("Invalid interface for session keys");
      return 1;
    }

  if (sodium_init() == -1) {
    E("Could not initialize libsodium");
    return 1;
  }

  uint8_t buffer[512]; /* Laaaaarge enough for the preamble and for each packet */

  if (read(fd, buffer, 16) != 16)
    {
      E("Header too small");
      return 1;
    }

  if (memcmp(buffer, MAGIC_NUMBER, 8) != 0)
    {
      E("Not a CRYPT4GH formatted file");
      return 2;
    }

  if (PEEK_U32_LE(buffer + 8) != VERSION)
    {
      E("Unsupported CRYPT4GH version");
      return 3;
    }

  int npackets = PEEK_U32_LE(buffer + 12);
  D1("Header contains %d packets", npackets);
  if (npackets == 0)
    {
      E("Empty Crypt4GH header");
      return 4;
    }

  int packet = 0, rc = 0;
  uint32_t packet_len = 0;

  /* Preallocate the sodium region (maybe one too much) */
  uint8_t* session_keys2 = (uint8_t*)sodium_malloc(CRYPT4GH_SESSION_KEY_SIZE * sizeof(uint8_t) * npackets);

  if(session_keys2 == NULL || errno == ENOMEM){
    D1("Could not allocate the buffer for the session keys");
    rc = 1;
    goto bail;
  }

  *session_keys = session_keys2;

  for (; packet < npackets; packet++)
    {
      D1("<<<<<<<<< Packet %d", packet);

      if (read(fd, buffer, 4) != 4){ /* overwrite buffer */
	E("Packet too small");
	rc = 5;
	goto bail;
      }

      packet_len = PEEK_U32_LE(buffer) - 4;
      if(packet_len > sizeof(buffer)) /* already >= 0 */
	{
	  D1("Invalid packet length %d", packet_len);
	  rc = 6;
	  goto bail;
	}

      if(read(fd, buffer, packet_len) != packet_len)/* overwrite buffer */
	{
	  D1("Packet %d too small", packet);
	  rc = 7;
	  goto bail;
	}

      unsigned int decrypted_len = packet_len - 4U - crypto_box_PUBLICKEYBYTES - CRYPT4GH_NONCE_SIZE;
      uint8_t decrypted[decrypted_len];
      /* memset(decrypted, '\0', decrypted_len); */

      if(header_decrypt_X25519_Chacha20_Poly1305(seckey, pubkey,
						  buffer, packet_len,
						  (uint8_t*)decrypted, &decrypted_len))
	{
	  D1("Cannot decrypt packet %d", packet);
	  continue;
	}

      /* valid session key or edit list */
      D3(">>>>>>>>> Packet %d decrypted [%u bytes]", packet, decrypted_len);
      H3("Packet", decrypted, decrypted_len);

      /* Parse the packet */
      rc = crypt4gh_packet_parse(decrypted, decrypted_len, &session_keys2, nkeys, edit_list, edit_list_len);
      sodium_memzero(decrypted, decrypted_len);

      if(rc){ D1("Invalid packet %d", packet); }
    }

bail:
  sodium_mprotect_readonly(session_keys);
  if(rc){ D1("Header Error %d", rc); }
  return rc;
}
