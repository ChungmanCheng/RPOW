/*
 * rpowutil.c
 *	Generate, read and write reusable proof of work tokens
 *
 * Copyright (C) 2004 Hal Finney
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <assert.h>

#include "rpowcli.h"
#include "hashcash.h"

#if defined(_WIN32)
#define ntohl(x) ((((x)>>24)&0xff)|(((x)>>8)&0xff00)| \
					(((x)&0xff00)<<8)|(((x)&0xff)<<24))
#define htonl	ntohl
#endif

/*
 * RPOW tokens come in two types.  In transit they are preceded by a type
 * byte and then a four byte value field, which is the equivalent of the
 * hashcash collision size, and must be in the range RPOW_VALUE_MIN to
 * RPOW_VALUE_MAX.  The hashcash type (type 2) then has a four byte length
 * field, and then a version 1 hashcash stamp.  The value in the stamp
 * should equal the earlier value field.
 *
 * The reusable type (type 1) then has a 20 byte keyid.  This is the hash of
 * the public key which issued the token.  It then has a 34 byte token id,
 * of which the last 14 bytes are the cardid where it can be exchanged.  Then
 * comes a value signed by the public key identified by the keyid.  The signed
 * value is in a bignum format where it is preceded by a 4-byte byte count.
 * The plaintext of that value consists of the 20 byte SHA-1 hash of the
 * token id, then the byte 2, then is padded to the width of the signing key's
 * modulus modulus.  The padding is done by repeatedly SHA-1 hashing what
 * we have so far and appending the hash, until we have the width we need
 * (the last append just uses the leftmost bytes of the hash).  We then
 * take that value mod the signing key's modulus.  This is what is signed.
 */

#define RPOW_PK_VAL		2
#define MIN(a,b)	((a)<(b)?(a):(b))
#define POW_EXPIRYSECONDS 14*86400
#define POW_GRACESECONDS 86400


int
issmallprime (int x)
{
	int p;

	if (x != 2  &&  (x & 1) == 0)
		return 0;
	for (p=3; p<=x/p ; p+=2)
		if (x % p == 0)
			return 0;
	return 1;
}


/* Find the exponent corresponding to the given value */
/* Exponents are consecutive primes starting with pk->e */
int
valuetoexp (gbignum *exp, int value, pubkey *pk)
{
	static int exptab[RPOW_VALUE_MAX-RPOW_VALUE_MIN+1];
	int i;

	if (exptab[0] == 0)
	{
		/* First time; fill exptab with consecutive primes */
		exptab[0] = gbig_to_word (&pk->e);
		for (i=1; i<sizeof(exptab)/sizeof(exptab[0]); i++)
		{
			exptab[i] = exptab[i-1] + 2;
			while (!issmallprime (exptab[i]))
			{
				exptab[i] += 2;
			}
		}
	}
	if (value < RPOW_VALUE_MIN || value > RPOW_VALUE_MAX)
		return -1;
	gbig_from_word (exp, exptab[value-RPOW_VALUE_MIN]);
	return 0;
}


/* Convert a regular hashcash coin to a buffer in our format */
uchar *
hc_to_buffer (char *buf, int *pbuflen)
{
	uchar *buf64;
	int buflen = *pbuflen;
	int value;

	while (isspace(buf[buflen-1]))
		--buflen;
	value = atoi (buf+2);
	buf64 = malloc (buflen + 9);
	buf64[0] = RPOW_TYPE_HASHCASH;
	buf64[1] = value >> 24;
	buf64[2] = value >> 16;
	buf64[3] = value >> 8;
	buf64[4] = value;
	buf64[5] = buflen >> 24;
	buf64[6] = buflen >> 16;
	buf64[7] = buflen >> 8;
	buf64[8] = buflen;
	memcpy (buf64+9, buf, buflen);
	buflen = buflen + 9;
	*pbuflen = buflen;
	return buf64;
}

/* Read an rpow value */
rpow *
rpow_read (rpowio *rpio)
{
	rpow *rp = calloc (sizeof(rpow), 1);
	int hclen;
	int value;

	gbig_init (&rp->bn);

	if (rp_read (rpio, &rp->type, 1) != 1)
		goto error;
	if (rp_read (rpio, &value, sizeof(value)) != sizeof(value))
		goto error;
	rp->value = ntohl (value);
	if (rp->value < RPOW_VALUE_MIN || rp->value > RPOW_VALUE_MAX)
		goto error;
	if (rp->type == RPOW_TYPE_HASHCASH)
	{
		if (rp_read (rpio, &hclen, sizeof(hclen)) != sizeof(hclen))
			goto error;
		rp->idlen = ntohl(hclen);
		rp->id = malloc (rp->idlen + 1);
		if (rp_read (rpio, rp->id, rp->idlen) != rp->idlen)
			goto error;
		rp->id[rp->idlen] = '\0';
	}
	else if (rp->type == RPOW_TYPE_RPOW)
	{
		if (rp_read (rpio, rp->keyid, KEYID_LENGTH) != KEYID_LENGTH)
			goto error;
		rp->id = malloc (RPOW_ID_LENGTH);
		if (rp_read (rpio, rp->id, RPOW_ID_LENGTH) != RPOW_ID_LENGTH)
			goto error;
		if (bnread (&rp->bn, rpio) < 0)
			goto error;
		rp->idlen = RPOW_ID_LENGTH;
	}
	else
		goto error;
	return rp;
error:
	gbig_free (&rp->bn);
	free (rp);
	return NULL;
}

/* Write out an rpow value */
int
rpow_write (rpow *rp, rpowio *rpio)
{
	int value = htonl(rp->value);
	if (rp_write (rpio, &rp->type, 1) != 1)
		return -1;
	if (rp_write (rpio, &value, sizeof(value)) != sizeof(value))
		return -1;
	if (rp->type == RPOW_TYPE_HASHCASH)
	{
		int hclen = htonl(rp->idlen);
		if (rp_write (rpio, &hclen, sizeof(hclen)) != sizeof(hclen))
			return -1;
		if (rp_write (rpio, rp->id, rp->idlen) != rp->idlen)
			return -1;
	} else {	/* rp->type == RPOW_TYPE_RPOW */
		if (rp_write (rpio, rp->keyid, KEYID_LENGTH) != KEYID_LENGTH)
			return -1;
		if (rp_write (rpio, rp->id, rp->idlen) != rp->idlen)
			return -1;
		if (bnwrite (&rp->bn, rpio) < 0)
			return -1;
	}
	return 0;
}


/* Free an rpow */
void
rpow_free (rpow *rp)
{
	if (rp->id)
		free (rp->id);
	gbig_free (&rp->bn);
	free (rp);
}

/* Return the POW resource name in a static buffer */
char *
powresource (unsigned char *cardid)
{
	static char resource[2*CARDID_LENGTH + 4 + sizeof(POW_RESOURCE_TAIL)];
	int i;

	resource[0] = 0;
	for (i=0; i<8; i++)
		sprintf (resource+strlen(resource), "%02x", cardid[i]);
	strcat (resource, "-");
	for (; i<12; i++)
		sprintf (resource+strlen(resource), "%02x", cardid[i]);
	strcat (resource, "-");
	for (; i<CARDID_LENGTH; i++)
		sprintf (resource+strlen(resource), "%02x", cardid[i]);
	strcat (resource, POW_RESOURCE_TAIL);
	return resource;
}

/* Generate a "hashcash" type of proof of work token */
rpow *
rpow_gen (int value, unsigned char *cardid)
{
	rpow *rp = calloc (sizeof(rpow), 1);
	char *resource = powresource(cardid);
	double tries;
	int ok;

	gbig_init (&rp->bn);

	if (value < RPOW_VALUE_MIN || value > RPOW_VALUE_MAX)
	{
		free (rp);
		return NULL;
	}

	rp->idlen = MAX_TOK;
	rp->id = malloc (rp->idlen);
	rp->value = value;

	ok = hashcash_mint1 (time(0), 0, resource, value, 0,
			rp->id, rp->idlen, NULL, &tries, NULL);
	assert (ok == 1);
	rp->idlen = strlen(rp->id);
	rp->type = RPOW_TYPE_HASHCASH;
	return rp;
}


/* Generate the rpow field of an rpowpend */
static void
rpowpend_bn_gen (gbignum *bn, uchar *id, unsigned idlen, pubkey *pk)
{
	uchar md[SHA_DIGEST_LENGTH];
	uchar *buf;
	int nlen = gbig_buflen (&pk->n);
	int off;

	buf = malloc (nlen);
	SHA1 (id, idlen, buf);
	buf[SHA_DIGEST_LENGTH] = RPOW_PK_VAL;
	off = SHA_DIGEST_LENGTH + 1;
	while (off < nlen)
	{
		SHA1 (buf, off, md);
		memcpy (buf+off, md, MIN(SHA_DIGEST_LENGTH, nlen-off));
		off += SHA_DIGEST_LENGTH;
	}
	gbig_from_buf (bn, buf, nlen);
	gbig_mod (bn, bn, &pk->n);
	free (buf);
}


/* Generate a value suitable for putting into an rpowpend */
rpowpend *
rpowpend_gen (int value, pubkey *pk)
{
	rpowpend *rpend = calloc (sizeof(rpowpend), 1);
	gbignum hider;
	gbignum ehider;
	gbignum exp;

	gbig_init (&hider);
	gbig_init (&ehider);
	gbig_init (&exp);
	gbig_init (&rpend->rpow);
	gbig_init (&rpend->rpowhidden);
	gbig_init (&rpend->invhider);

	if (valuetoexp (&exp, value, pk) < 0)
		return NULL;

	rpend->value = value;
	rpend->idlen = RPOW_ID_LENGTH;
	gbig_rand_bytes (rpend->id, rpend->idlen - CARDID_LENGTH);
	memcpy (rpend->id + rpend->idlen - CARDID_LENGTH, pk->cardid,
			CARDID_LENGTH);
	rpowpend_bn_gen (&rpend->rpow, rpend->id, rpend->idlen, pk);
	gbig_init (&rpend->rpowhidden);
	gbig_init (&rpend->invhider);
	gbig_copy (&rpend->rpowhidden, &rpend->rpow);
	gbig_from_word (&rpend->invhider, 1);
	gbig_free (&exp);
	gbig_free (&hider);
	gbig_free (&ehider);
	return rpend;
}

/* Read an rpowpend written by rpowpend_write */
rpowpend *
rpowpend_read (rpowio *rpio)
{
	rpowpend *rpend = calloc (sizeof(rpowpend), 1);
	int value;

	gbig_init (&rpend->rpow);
	gbig_init (&rpend->rpowhidden);
	gbig_init (&rpend->invhider);

	rp_read (rpio, &value, sizeof(value));
	rpend->value = ntohl(value);
	if (rpend->value < RPOW_VALUE_MIN || rpend->value > RPOW_VALUE_MAX)
		goto error;
	if (bnread (&rpend->rpow, rpio) < 0)
		goto error;
	return rpend;
error:
	gbig_free (&rpend->rpow);
	gbig_free (&rpend->rpowhidden);
	gbig_free (&rpend->invhider);
	free (rpend);
	return NULL;
}

/* Write out an rpowpend */
int
rpowpend_write (rpowpend *rpend, rpowio *rpio)
{
	int value = htonl(rpend->value);
	if (rp_write (rpio, &value, sizeof(value)) != sizeof(value))
		return -1;
	if (bnwrite (&rpend->rpowhidden, rpio) < 0)
		return -1;
	return 0;
}

/* Read and validate a signed rpowpend from the server, producing a new rpow */
rpow *
rpowpend_rpow (rpowpend *rpend, pubkey *pk, rpowio *rpio)
{
	rpow *rp = calloc (sizeof(rpow), 1);
	gbignum tmp1;
	gbignum exp;

	gbig_init (&tmp1);
	gbig_init (&exp);
	gbig_init (&rp->bn);

	if (valuetoexp (&exp, rpend->value, pk) < 0)
		goto error;

	if (bnread (&rp->bn, rpio) < 0)
		goto error;
	gbig_mod_mul (&rp->bn, &rp->bn, &rpend->invhider, &pk->n);

	/* Validate signature */
	gbig_mod_exp (&tmp1, &rp->bn, &exp, &pk->n);
	if (gbig_cmp (&tmp1, &rpend->rpow) != 0)
		goto error;

	rp->idlen = rpend->idlen;
	rp->id = malloc (rp->idlen);
	memcpy (rp->id, rpend->id, rpend->idlen);
	rp->type = RPOW_TYPE_RPOW;
	rp->value = rpend->value;
	memcpy (rp->keyid, pk->keyid, sizeof(rp->keyid));
	gbig_free (&tmp1);
	gbig_free (&exp);
	return rp;
error:
	gbig_free (&rp->bn);
	free (rp);
	gbig_free (&tmp1);
	gbig_free (&exp);
	return NULL;
}

/* Free an rpowpend */
void
rpowpend_free (rpowpend *rpend)
{
	gbig_free (&rpend->rpow);
	gbig_free (&rpend->rpowhidden);
	gbig_free (&rpend->invhider);
	free (rpend);
}


#if 0
/* Validate an rpow token, return 0 if OK, error code if bad */
int
rpow_validate (rpow *rp)
{
	if (rp->type == RPOW_TYPE_HASHCASH)
		return rpow_valid_pow (rp);
	else
		return rpow_valid_pk (rp);
}

/* Given a POW token (hashcash version 1), parse out the fields */
/* Example:  1:15:040719:rpow.net::9e6c82f8e4727a6d:1ec4 */
/* The pointers returned are pointers into the input str */
/* str does not have to be null terminated */
/* Return error if no good */
#define MAXFIELDS	6
static int
pow_parse (const char *str, int len, int *pvalue, time_t *ptime,
		char **presource, char **pparams)
{
	static char str2[MAX_TOK];
	char *pstr = str2;
	char *field[MAXFIELDS];
	int nfields = 0;
	int timelen;
	struct tm powtm;
	char *powtime;
	char tbuf[3];

	if (len > MAX_TOK || len < MAXFIELDS
				|| str[0] != '1' || str[1] != ':')
		return RPOW_STAT_INVALID;

	strncpy (str2, str, len);

	while (len--)
	{
		if (*pstr == '\0')
			return RPOW_STAT_INVALID;
		if (*pstr == ':')
		{
			if (nfields+1 > MAXFIELDS)
				return RPOW_STAT_INVALID;
			field[nfields++] = pstr+1;
			*pstr = '\0';
		}
		++pstr;
	}

	if (nfields != MAXFIELDS)
		return RPOW_STAT_INVALID;

	powtime = field[1];
	timelen = strlen (powtime);
	if (timelen < 6)
		return RPOW_STAT_INVALID;

	if (pvalue)
		*pvalue = atoi(field[0]);
	if (presource)
		*presource = field[2];
	if (pparams)
		*pparams = field[3];
	if (ptime)
	{
		memset (&powtm, 0, sizeof(powtm));
		memset (tbuf, 0, sizeof(tbuf));
		strncpy (tbuf, powtime, 2);
		powtm.tm_year = atoi(tbuf) + 100;
		strncpy (tbuf, powtime+2, 2);
		powtm.tm_mon = atoi(tbuf) - 1;
		strncpy (tbuf, powtime+4, 2);
		powtm.tm_mday = atoi(tbuf);
		*ptime = mktime(&powtm);
	}
	return RPOW_STAT_OK;
}

int
rpow_valid_pow (rpow *rp)
{
	int rslt;
	time_t nowtime, powtime;
	int powvalue;
	char *powresource1;
	uchar md[SHA1_DIGEST_LENGTH];
	int i;

	if (rp->value < RPOW_VALUE_MIN || rp->value > RPOW_VALUE_MAX)
		return RPOW_STAT_INVALID;

	/* Parse the POW and see if its fields are legal */
	rslt = pow_parse (rp->id, rp->idlen, &powvalue, &powtime,
				&powresource, NULL);
	if (rslt < 0)
		return rslt;
	if (powvalue != rp->value)
		return RPOW_STAT_INVALID;
	if (strcmp (powresource1, powresource) != 0)
		return RPOW_STAT_BADRESOURCE;
	nowtime = time(0);
	if (powtime > nowtime + POW_GRACESECONDS ||
			powtime < nowtime - POW_EXPIRYSECONDS)
		return RPOW_STAT_BADTIME;

	/* Now test the hash to see if it has the right number of high 0's */
	gbig_sha1_buf (md, rp->id, rp->idlen);
	for (i=0; (i+1)*8<powvalue; i++)
		if (md[i] != 0)
			return RPOW_STAT_INVALID;
	if (md[i] & ~(0xff >> (powvalue&7)))
		return RPOW_STAT_INVALID;

	return RPOW_STAT_OK;
}

/* Temporary version of this, return the one public key we know about */
static struct pubkey *
pk_from_keyid (unsigned char *keyid)
{
	extern pubkey signpubkey;
	if (memcmp (signpubkey.keyid, keyid, KEYID_LENGTH) != 0)
		return NULL;
	return &signpubkey;
}

int
rpow_valid_pk (rpow *rp)
{
	pubkey *pk;
	gbignum paddedid;
	gbignum pow;
	gbignum exp;
	int stat = RPOW_STAT_OK;

	if ((pk = pk_from_keyid (rp->keyid)) == NULL)
		return RPOW_STAT_WRONGKEY;

	gbig_init (&paddedid);
	gbig_init (&pow);
	gbig_init (&exp);

	if (valuetoexp (&exp, rp->value, pk) < 0)
	{
		stat = RPOW_STAT_INVALID;
		goto done;
	}
	gbig_mod_exp (&pow, &rp->bn, &exp, &pk->n);
	rpowpend_bn_gen (&paddedid, rp->id, rp->idlen, pk);
	if (gbig_cmp (&pow, &paddedid) != 0)
		stat = RPOW_STAT_INVALID;
done:
	gbig_free (&exp);
	gbig_free (&pow);
	gbig_free (&paddedid);
	return stat;
}
#endif