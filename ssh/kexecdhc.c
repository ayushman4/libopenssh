/* $OpenBSD: kexecdhc.c,v 1.2 2010/09/22 05:01:29 djm Exp $ */
/*
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
 * Copyright (c) 2010 Damien Miller.  All rights reserved.
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

#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <openssl/ecdh.h>

#include "key.h"
#include "cipher.h"
#include "kex.h"
#include "log.h"
#include "packet.h"
#include "dh.h"
#include "ssh2.h"
#include "dispatch.h"
#include "compat.h"
#include "err.h"
#include "sshbuf.h"

static int input_kex_ecdh_reply(int, u_int32_t, struct ssh *);

int
kexecdh_client(struct ssh *ssh)
{
	struct kex *kex = ssh->kex;
	EC_KEY *client_key = NULL;
	const EC_GROUP *group;
	const EC_POINT *public_key;
	int r, curve_nid;

	if ((curve_nid = kex_ecdh_name_to_nid(kex->name)) == -1) {
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}
	if ((client_key = EC_KEY_new_by_curve_name(curve_nid)) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	if (EC_KEY_generate_key(client_key) != 1) {
		r = SSH_ERR_LIBCRYPTO_ERROR;
		goto out;
	}
	group = EC_KEY_get0_group(client_key);
	public_key = EC_KEY_get0_public_key(client_key);

	if ((r = sshpkt_start(ssh, SSH2_MSG_KEX_ECDH_INIT)) != 0 ||
	    (r = sshpkt_put_ec(ssh, public_key, group)) != 0 ||
	    (r = sshpkt_send(ssh)) != 0)
		goto out;
	debug("sending SSH2_MSG_KEX_ECDH_INIT");

#ifdef DEBUG_KEXECDH
	fputs("client private key:\n", stderr);
	sshkey_dump_ec_key(client_key);
#endif
	kex->ec_client_key = client_key;
	kex->ec_group = group;
	client_key = NULL;	/* owned by the kex */

	debug("expecting SSH2_MSG_KEX_ECDH_REPLY");
	ssh_dispatch_set(ssh, SSH2_MSG_KEX_ECDH_REPLY, &input_kex_ecdh_reply);
	r = 0;
 out:
	if (client_key)
		EC_KEY_free(client_key);
	return r;
}

static int
input_kex_ecdh_reply(int type, u_int32_t seq, struct ssh *ssh)
{
	struct kex *kex = ssh->kex;
	const EC_GROUP *group;
	EC_POINT *server_public = NULL;
	EC_KEY *client_key;
	BIGNUM *shared_secret = NULL;
	struct sshkey *server_host_key = NULL;
	u_char *server_host_key_blob = NULL, *signature = NULL;
	u_char *kbuf = NULL, *hash;
	size_t slen, sbloblen;
	size_t klen = 0, hashlen;
	int r;

	if (kex->verify_host_key == NULL) {
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}
	group = kex->ec_group;
	client_key = kex->ec_client_key;

	/* hostkey */
	if ((r = sshpkt_get_string(ssh, &server_host_key_blob,
	    &sbloblen)) != 0 ||
	    (r = sshkey_from_blob(server_host_key_blob, sbloblen,
	    &server_host_key)) != 0)
		goto out;
	if (server_host_key->type != kex->hostkey_type) {
		r = SSH_ERR_KEY_TYPE_MISMATCH;
		goto out;
	}
	if (kex->verify_host_key(server_host_key, ssh) == -1) {
		r = SSH_ERR_SIGNATURE_INVALID;
		goto out;
	}

	/* Q_S, server public key */
	/* signed H */
	if ((server_public = EC_POINT_new(group)) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	if ((r = sshpkt_get_ec(ssh, server_public, group)) != 0 ||
	    (r = sshpkt_get_string(ssh, &signature, &slen)) != 0 ||
	    (r = sshpkt_get_end(ssh)) != 0)
		goto out;

#ifdef DEBUG_KEXECDH
	fputs("server public key:\n", stderr);
	sshkey_dump_ec_point(group, server_public);
#endif
	if (sshkey_ec_validate_public(group, server_public) != 0) {
		sshpkt_disconnect(ssh, "invalid server public key");
		r = SSH_ERR_MESSAGE_INCOMPLETE;
		goto out;
	}

	klen = (EC_GROUP_get_degree(group) + 7) / 8;
	if ((kbuf = malloc(klen)) == NULL ||
	    (shared_secret = BN_new()) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	if (ECDH_compute_key(kbuf, klen, server_public,
	    client_key, NULL) != (int)klen ||
	    BN_bin2bn(kbuf, klen, shared_secret) == NULL) {
		r = SSH_ERR_LIBCRYPTO_ERROR;
		goto out;
	}

#ifdef DEBUG_KEXECDH
	dump_digest("shared secret", kbuf, klen);
#endif
	/* calc and verify H */
	if ((r = kex_ecdh_hash(
	    kex->evp_md,
	    group,
	    kex->client_version_string,
	    kex->server_version_string,
	    sshbuf_ptr(kex->my), sshbuf_len(kex->my),
	    sshbuf_ptr(kex->peer), sshbuf_len(kex->peer),
	    server_host_key_blob, sbloblen,
	    EC_KEY_get0_public_key(client_key),
	    server_public,
	    shared_secret,
	    &hash, &hashlen)) != 0)
		goto out;

	if ((r = sshkey_verify(server_host_key, signature, slen, hash,
	    hashlen, ssh->compat)) != 0)
		goto out;

	/* save session id */
	if (kex->session_id == NULL) {
		kex->session_id_len = hashlen;
		kex->session_id = malloc(kex->session_id_len);
		if (kex->session_id == NULL) {
			r = SSH_ERR_ALLOC_FAIL;
			goto out;
		}
		memcpy(kex->session_id, hash, kex->session_id_len);
	}

	if ((r = kex_derive_keys(ssh, hash, hashlen, shared_secret)) == 0)
		r = kex_send_newkeys(ssh);
 out:
	if (kex->ec_client_key) {
		EC_KEY_free(kex->ec_client_key);
		kex->ec_client_key = NULL;
	}
	if (server_host_key_blob)
		free(server_host_key_blob);
	if (server_host_key)
		sshkey_free(server_host_key);
	if (server_public)
		EC_POINT_clear_free(server_public);
	if (kbuf) {
		bzero(kbuf, klen);
		free(kbuf);
	}
	if (shared_secret)
		BN_clear_free(shared_secret);
	if (signature)
		free(signature);
	return r;
}
