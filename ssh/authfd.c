/* $OpenBSD: authfd.c,v 1.86 2011/07/06 18:09:21 tedu Exp $ */
/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Functions for connecting the local authentication agent.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 *
 * SSH2 implementation,
 * Copyright (c) 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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


#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>

#include <openssl/evp.h>

#include <openssl/crypto.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "xmalloc.h"
#include "ssh.h"
#include "rsa.h"
#include "sshbuf.h"
#include "key.h"
#include "authfd.h"
#include "cipher.h"
#include "kex.h"
#include "compat.h"
#include "log.h"
#include "atomicio.h"
#include "misc.h"
#include "err.h"

#define MAX_AGENT_IDENTITIES	2048		/* Max keys in agent reply */
#define MAX_AGENT_REPLY_LEN	(256 * 1024) 	/* Max bytes in agent reply */

/* macro to check for "agent failure" message */
#define agent_failed(x) \
    ((x == SSH_AGENT_FAILURE) || \
    (x == SSH_COM_AGENT2_FAILURE) || \
    (x == SSH2_AGENT_FAILURE))

/* Convert success/failure response from agent to a err.h status */
static int
decode_reply(u_char type)
{
	if (agent_failed(type))
		return SSH_ERR_AGENT_FAILURE;
	else if (type == SSH_AGENT_SUCCESS)
		return 0;
	else
		return SSH_ERR_INVALID_FORMAT;
}

/* Returns the number of the authentication fd, or -1 if there is none. */
int
ssh_get_authentication_socket(int *fdp)
{
	const char *authsocket;
	int sock, oerrno;
	struct sockaddr_un sunaddr;

	if (fdp != NULL)
		*fdp = -1;

	authsocket = getenv(SSH_AUTHSOCKET_ENV_NAME);
	if (!authsocket)
		return SSH_ERR_AGENT_NOT_PRESENT;

	bzero(&sunaddr, sizeof(sunaddr));
	sunaddr.sun_family = AF_UNIX;
	strlcpy(sunaddr.sun_path, authsocket, sizeof(sunaddr.sun_path));

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return SSH_ERR_SYSTEM_ERROR;

	/* close on exec */
	if (fcntl(sock, F_SETFD, FD_CLOEXEC) == -1 ||
	    connect(sock, (struct sockaddr *)&sunaddr, sizeof(sunaddr)) < 0) {
		oerrno = errno;
		close(sock);
		errno = oerrno;
		return SSH_ERR_SYSTEM_ERROR;
	}
	if (fdp != NULL)
		*fdp = sock;
	else
		close(sock);
	return 0;
}

/* Communicate with agent: send request and read reply */
static int
ssh_request_reply(int sock, struct sshbuf *request, struct sshbuf *reply)
{
	int r;
	size_t l, len;
	char buf[1024];

	/* Get the length of the message, and format it in the buffer. */
	len = sshbuf_len(request);
	put_u32(buf, len);

	/* Send the length and then the packet to the agent. */
	if (atomicio(vwrite, sock, buf, 4) != 4 ||
	    atomicio(vwrite, sock, sshbuf_ptr(request),
	    sshbuf_len(request)) != sshbuf_len(request))
		return SSH_ERR_AGENT_COMMUNICATION;
	/*
	 * Wait for response from the agent.  First read the length of the
	 * response packet.
	 */
	if (atomicio(read, sock, buf, 4) != 4)
	    return SSH_ERR_AGENT_COMMUNICATION;

	/* Extract the length, and check it for sanity. */
	len = get_u32(buf);
	if (len > MAX_AGENT_REPLY_LEN)
		return SSH_ERR_INVALID_FORMAT;

	/* Read the rest of the response in to the buffer. */
	sshbuf_reset(reply);
	while (len > 0) {
		l = len;
		if (l > sizeof(buf))
			l = sizeof(buf);
		if (atomicio(read, sock, buf, l) != l)
			return SSH_ERR_AGENT_COMMUNICATION;
		if ((r = sshbuf_put(reply, buf, l)) != 0)
			return r;
		len -= l;
	}
	return 0;
}

/*
 * Closes the agent socket if it should be closed (depends on how it was
 * obtained).  The argument must have been returned by
 * ssh_get_authentication_socket().
 */
void
ssh_close_authentication_socket(int sock)
{
	if (getenv(SSH_AUTHSOCKET_ENV_NAME))
		close(sock);
}

/* Lock/unlock agent */
int
ssh_lock_agent(int sock, int lock, const char *password)
{
	int r;
	u_char type = lock ? SSH_AGENTC_LOCK : SSH_AGENTC_UNLOCK;
	struct sshbuf *msg;

	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshbuf_put_u8(msg, type)) != 0 ||
	    (r = sshbuf_put_cstring(msg, password)) != 0)
		goto out;
	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	r = decode_reply(type);
 out:
	sshbuf_free(msg);
	return r;
}

static int
deserialise_identity1(struct sshbuf *ids, struct sshkey **keyp, char **commentp)
{
	struct sshkey *key;
	int r, keybits;
	u_int32_t bits;
	char *comment = NULL;

	if ((key = sshkey_new(KEY_RSA1)) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshbuf_get_u32(ids, &bits)) != 0 ||
	    (r = sshbuf_get_bignum1(ids, key->rsa->e)) != 0 ||
	    (r = sshbuf_get_bignum1(ids, key->rsa->n)) != 0 ||
	    (r = sshbuf_get_cstring(ids, &comment, NULL)) != 0)
		goto out;
	keybits = BN_num_bits(key->rsa->n);
	/* XXX previously we just warned here. I think we should be strict */
	if (keybits < 0 || bits != (u_int)keybits) {
		r = SSH_ERR_KEY_BITS_MISMATCH;
		goto out;
	}
	if (keyp != NULL) {
		*keyp = key;
		key = NULL;
	}
	if (commentp != NULL) {
		*commentp = comment;
		comment = NULL;
	}
	r = 0;
 out:
	sshkey_free(key);
	free(comment);
	return r;
}

static int
deserialise_identity2(struct sshbuf *ids, struct sshkey **keyp, char **commentp)
{
	int r;
	char *comment = NULL;
	u_char *blob = NULL;
	size_t blen;

	if ((r = sshbuf_get_string(ids, &blob, &blen)) != 0 ||
	    (r = sshbuf_get_cstring(ids, &comment, NULL)) != 0)
		goto out;
	if ((r = sshkey_from_blob(blob, blen, keyp)) != 0)
		goto out;
	if (commentp != NULL) {
		*commentp = comment;
		comment = NULL;
	}
	r = 0;
 out:
	if (blob != NULL) {
		bzero(blob, blen);
		free(blob);
	}
	free(comment);
	return r;
}

/*
 * Fetch list of identities held by the agent.
 */
int
ssh_fetch_identitylist(int sock, int version, struct ssh_identitylist **idlp)
{
	u_char type, code1 = 0, code2 = 0;
	u_int32_t num, i;
	struct sshbuf *msg;
	struct ssh_identitylist *idl = NULL;
	int r;

	/* Determine request and expected response types */
	switch (version) {
	case 1:
		code1 = SSH_AGENTC_REQUEST_RSA_IDENTITIES;
		code2 = SSH_AGENT_RSA_IDENTITIES_ANSWER;
		break;
	case 2:
		code1 = SSH2_AGENTC_REQUEST_IDENTITIES;
		code2 = SSH2_AGENT_IDENTITIES_ANSWER;
		break;
	default:
		return SSH_ERR_INVALID_ARGUMENT;
	}

	/*
	 * Send a message to the agent requesting for a list of the
	 * identities it can represent.
	 */
	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshbuf_put_u8(msg, code1)) != 0)
		goto out;

	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;

	/* Get message type, and verify that we got a proper answer. */
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	if (agent_failed(type)) {
		r = SSH_ERR_AGENT_FAILURE;
		goto out;
	} else if (type != code2) {
		r = SSH_ERR_INVALID_FORMAT;
		goto out;
	}

	/* Get the number of entries in the response and check it for sanity. */
	if ((r = sshbuf_get_u32(msg, &num)) != 0)
		goto out;
	if (num > MAX_AGENT_IDENTITIES) {
		r = SSH_ERR_INVALID_FORMAT;
		goto out;
	}
	if (num == 0) {
		r = SSH_ERR_AGENT_NO_IDENTITIES;
		goto out;
	}

	/* Deserialise the response into a list of keys/comments */
	if ((idl = calloc(1, sizeof(*idl))) == NULL ||
	    (idl->keys = calloc(num, sizeof(*idl->keys))) == NULL ||
	    (idl->comments = calloc(num, sizeof(*idl->comments))) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	for (i = 0; i < num; i++) {
		switch (version) {
		case 1:
			if ((r = deserialise_identity1(msg,
			    &(idl->keys[i]), &(idl->comments[i]))) != 0)
				goto out;
			break;
		case 2:
			if ((r = deserialise_identity2(msg,
			    &(idl->keys[i]), &(idl->comments[i]))) != 0)
				goto out;
			break;
		}
	}
	idl->nkeys = num;
	*idlp = idl;
	idl = NULL;
	r = 0;
 out:
	sshbuf_free(msg);
	if (idl != NULL)
		ssh_free_identitylist(idl);
	return r;
}

void
ssh_free_identitylist(struct ssh_identitylist *idl)
{
	size_t i;

	if (idl == NULL)
		return;
	for (i = 0; i < idl->nkeys; i++) {
		if (idl->keys != NULL)
			sshkey_free(idl->keys[i]);
		if (idl->comments != NULL)
			free(idl->comments[i]);
	}
	free(idl);
}

/*
 * Sends a challenge (typically from a server via ssh(1)) to the agent,
 * and waits for a response from the agent.
 * Returns true (non-zero) if the agent gave the correct answer, zero
 * otherwise.
 */
int
ssh_decrypt_challenge(int sock, struct sshkey* key, BIGNUM *challenge,
    u_char session_id[16], u_char response[16])
{
	struct sshbuf *msg;
	int r;
	u_char type;

	if (key->type != KEY_RSA1)
		return SSH_ERR_INVALID_ARGUMENT;
	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshbuf_put_u8(msg, SSH_AGENTC_RSA_CHALLENGE)) != 0 ||
	    (r = sshbuf_put_u32(msg, BN_num_bits(key->rsa->n))) != 0 ||
	    (r = sshbuf_put_bignum1(msg, key->rsa->e)) != 0 ||
	    (r = sshbuf_put_bignum1(msg, key->rsa->n)) != 0 ||
	    (r = sshbuf_put_bignum1(msg, challenge)) != 0 ||
	    (r = sshbuf_put(msg, session_id, sizeof(session_id))) != 0 ||
	    (r = sshbuf_put_u32(msg, 1)) != 0) /* Response type for proto 1.1 */
		goto out;
	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	if (agent_failed(type)) {
		r = SSH_ERR_AGENT_FAILURE;
		goto out;
	} else if (type != SSH_AGENT_RSA_RESPONSE) {
		r = SSH_ERR_INVALID_FORMAT;
		goto out;
	}
	if ((r = sshbuf_get(msg, response, sizeof(response))) != 0)
		goto out;
	r = 0;
 out:
	sshbuf_free(msg);
	return r;
}

/* ask agent to sign data, returns err.h code on error, 0 on success */
int
ssh_agent_sign(int sock, struct sshkey *key,
    u_char **sigp, u_int *lenp,
    u_char *data, u_int datalen, u_int compat)
{
	struct sshbuf *msg;
	u_char *blob;
	u_int blen, flags = 0;
	u_char type;
	size_t len;
	int r;

	if (compat & SSH_BUG_SIGBLOB)
		flags |= SSH_AGENT_OLD_SIGNATURE;
	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshkey_to_blob(key, &blob, &blen)) != 0)
		goto out;
	if ((r = sshbuf_put_u8(msg, SSH2_AGENTC_SIGN_REQUEST)) != 0 ||
	    (r = sshbuf_put_string(msg, blob, blen)) != 0 ||
	    (r = sshbuf_put_string(msg, data, datalen)) != 0 ||
	    (r = sshbuf_put_u32(msg, flags)) != 0)
		goto out;
	if ((r = ssh_request_reply(sock, msg, msg) != 0))
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	if (agent_failed(type)) {
		r = SSH_ERR_AGENT_FAILURE;
		goto out;
	} else if (type != SSH2_AGENT_SIGN_RESPONSE) {
		r = SSH_ERR_INVALID_FORMAT;
		goto out;
	}
	if ((r = sshbuf_get_string(msg, sigp, &len)) != 0)
		goto out;
	*lenp = len;
	r = 0;
 out:
	if (blob != NULL) {
		bzero(blob, blen);
		free(blob);
	}
	sshbuf_free(msg);
	return r;
}

/* Encode key for a message to the agent. */

static int
ssh_encode_identity_rsa1(struct sshbuf *b, RSA *key, const char *comment)
{
	int r;

	/* To keep within the protocol: p < q for ssh. in SSL p > q */
	if ((r = sshbuf_put_u32(b, BN_num_bits(key->n))) != 0 ||
	    (r = sshbuf_put_bignum1(b, key->n)) != 0 ||
	    (r = sshbuf_put_bignum1(b, key->e)) != 0 ||
	    (r = sshbuf_put_bignum1(b, key->d)) != 0 ||
	    (r = sshbuf_put_bignum1(b, key->iqmp)) != 0 ||
	    (r = sshbuf_put_bignum1(b, key->q)) != 0 ||
	    (r = sshbuf_put_bignum1(b, key->p)) != 0 ||
	    (r = sshbuf_put_cstring(b, comment)) != 0)
		return r;
	return 0;
}

static int
ssh_encode_identity_ssh2(struct sshbuf *b, struct sshkey *key,
    const char *comment)
{
	int r;

	if ((r = sshbuf_put_cstring(b, sshkey_ssh_name(key))) != 0)
		return r;
	switch (key->type) {
	case KEY_RSA:
		if ((r = sshbuf_put_bignum2(b, key->rsa->n)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->e)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->d)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->iqmp)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->p)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->q)) != 0)
			return r;
		break;
	case KEY_RSA_CERT_V00:
	case KEY_RSA_CERT:
		if (key->cert == NULL || sshbuf_len(key->cert->certblob) == 0)
			return SSH_ERR_INVALID_ARGUMENT;
		if ((r = sshbuf_put_stringb(b, key->cert->certblob)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->d)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->iqmp)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->p)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->rsa->q)) != 0)
			return r;
		break;
	case KEY_DSA:
		if ((r = sshbuf_put_bignum2(b, key->dsa->p)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->dsa->q)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->dsa->g)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->dsa->pub_key)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->dsa->priv_key)) != 0)
			return r;
		break;
	case KEY_DSA_CERT_V00:
	case KEY_DSA_CERT:
		if (key->cert == NULL || sshbuf_len(key->cert->certblob) == 0)
			return SSH_ERR_INVALID_ARGUMENT;
		if ((r = sshbuf_put_stringb(b, key->cert->certblob)) != 0 ||
		    (r = sshbuf_put_bignum2(b, key->dsa->priv_key)) != 0)
			return r;
		break;
	case KEY_ECDSA:
		if ((r = sshbuf_put_cstring(b,
		    sshkey_curve_nid_to_name(key->ecdsa_nid))) != 0 ||
		    (r = sshbuf_put_eckey(b, key->ecdsa)) != 0 ||
		    (r = sshbuf_put_bignum2(b,
		    EC_KEY_get0_private_key(key->ecdsa))) != 0)
			return r;
		break;
	case KEY_ECDSA_CERT:
		if (key->cert == NULL || sshbuf_len(key->cert->certblob) == 0)
			return SSH_ERR_INVALID_ARGUMENT;
		if ((r = sshbuf_put_stringb(b, key->cert->certblob)) != 0 ||
		    (r = sshbuf_put_bignum2(b,
		    EC_KEY_get0_private_key(key->ecdsa))) != 0)
			return r;
		break;
	}
	if ((r = sshbuf_put_cstring(b, comment)) != 0)
		return r;
	return 0;
}

static int
encode_constraints(struct sshbuf *m, u_int life, u_int confirm)
{
	int r;

	if (life != 0) {
		if ((r = sshbuf_put_u8(m, SSH_AGENT_CONSTRAIN_LIFETIME)) != 0 ||
		    (r = sshbuf_put_u32(m, life)) != 0)
			goto out;
	}
	if (confirm != 0) {
		if ((r = sshbuf_put_u8(m, SSH_AGENT_CONSTRAIN_CONFIRM)) != 0)
			goto out;
	}
	r = 0;
 out:
	return r;
}

/*
 * Adds an identity to the authentication server.
 * This call is intended only for use by ssh-add(1) and like applications.
 */
int
ssh_add_identity_constrained(int sock, struct sshkey *key, const char *comment,
    u_int life, u_int confirm)
{
	struct sshbuf *msg;
	int r, constrained = (life || confirm);
	u_char type;

	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;

	switch (key->type) {
	case KEY_RSA1:
		type = constrained ?
		    SSH_AGENTC_ADD_RSA_ID_CONSTRAINED :
		    SSH_AGENTC_ADD_RSA_IDENTITY;
		if ((r = sshbuf_put_u8(msg, type)) != 0 ||
		    (r = ssh_encode_identity_rsa1(msg, key->rsa, comment)) != 0)
			goto out;
		break;
	case KEY_RSA:
	case KEY_RSA_CERT:
	case KEY_RSA_CERT_V00:
	case KEY_DSA:
	case KEY_DSA_CERT:
	case KEY_DSA_CERT_V00:
	case KEY_ECDSA:
	case KEY_ECDSA_CERT:
		type = constrained ?
		    SSH2_AGENTC_ADD_ID_CONSTRAINED :
		    SSH2_AGENTC_ADD_IDENTITY;
		if ((r = sshbuf_put_u8(msg, type)) != 0 ||
		    (r = ssh_encode_identity_ssh2(msg, key, comment)) != 0)
			goto out;
	default:
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}
	if (constrained &&
	    (r = encode_constraints(msg, life, confirm)) != 0)
		goto out;
	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	r = decode_reply(type);
 out:
	sshbuf_free(msg);
	return r;
}

/*
 * Removes an identity from the authentication server.
 * This call is intended only for use by ssh-add(1) and like applications.
 */
int
ssh_remove_identity(int sock, struct sshkey *key)
{
	struct sshbuf *msg;
	int r;
	u_char type, *blob = NULL;
	u_int blen;

	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;

	if (key->type == KEY_RSA1) {
		if ((r = sshbuf_put_u8(msg,
		    SSH_AGENTC_REMOVE_RSA_IDENTITY)) != 0 ||
		    (r = sshbuf_put_u32(msg, BN_num_bits(key->rsa->n))) != 0 ||
		    (r = sshbuf_put_bignum1(msg, key->rsa->e)) != 0 ||
		    (r = sshbuf_put_bignum1(msg, key->rsa->n)) != 0)
			goto out;
	} else if (sshkey_type_plain(key->type) == KEY_DSA ||
	    sshkey_type_plain(key->type) == KEY_RSA ||
	    sshkey_type_plain(key->type) == KEY_ECDSA) {
		if ((r = sshkey_to_blob(key, &blob, &blen)) != 0)
			goto out;
		if ((r = sshbuf_put_u8(msg,
		    SSH2_AGENTC_REMOVE_IDENTITY)) != 0 ||
		    (r = sshbuf_put_string(msg, blob, blen)) != 0)
			goto out;
	} else {
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}
	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	r = decode_reply(type);
 out:
	if (blob != NULL) {
		bzero(blob, blen);
		free(blob);
	}
	sshbuf_free(msg);
	return r;
}

/*
 * Add/remove an token-based identity from the authentication server.
 * This call is intended only for use by ssh-add(1) and like applications.
 */
int
ssh_update_card(int sock, int add, const char *reader_id, const char *pin,
    u_int life, u_int confirm)
{
	struct sshbuf *msg;
	int r, constrained = (life || confirm);
	u_char type;

	if (add) {
		type = constrained ?
		    SSH_AGENTC_ADD_SMARTCARD_KEY_CONSTRAINED :
		    SSH_AGENTC_ADD_SMARTCARD_KEY;
	} else
		type = SSH_AGENTC_REMOVE_SMARTCARD_KEY;

	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshbuf_put_u8(msg, type)) != 0 ||
	    (r = sshbuf_put_cstring(msg, reader_id)) != 0 ||
	    (r = sshbuf_put_cstring(msg, pin)) != 0)
		goto out;
	if (constrained &&
	    (r = encode_constraints(msg, life, confirm)) != 0)
		goto out;
	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	r = decode_reply(type);
 out:
	sshbuf_free(msg);
	return r;
}

/*
 * Removes all identities from the agent.
 * This call is intended only for use by ssh-add(1) and like applications.
 */
int
ssh_remove_all_identities(int sock, int version)
{
	struct sshbuf *msg;
	u_char type = (version == 1) ?
	    SSH_AGENTC_REMOVE_ALL_RSA_IDENTITIES :
	    SSH2_AGENTC_REMOVE_ALL_IDENTITIES;
	int r;

	if ((msg = sshbuf_new()) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	if ((r = sshbuf_put_u8(msg, type)) != 0)
		goto out;
	if ((r = ssh_request_reply(sock, msg, msg)) != 0)
		goto out;
	if ((r = sshbuf_get_u8(msg, &type)) != 0)
		goto out;
	r = decode_reply(type);
 out:
	sshbuf_free(msg);
	return r;
}
