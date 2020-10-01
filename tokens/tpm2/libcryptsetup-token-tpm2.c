/*
 * LUKS - Linux Unified Key Setup v2, TPM type token handler
 *
 * Copyright (C) 2018-2020 Fraunhofer SIT sponsorred by Infineon Technologies AG
 * Copyright (C) 2019-2020 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2019-2020 Daniel Zatovic
 * Copyright (C) 2019-2020 Milan Broz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "utils_tpm2.h"
#include "libcryptsetup.h"
#include "../../src/plugin.h"

#define TOKEN_NAME "tpm2"
#define TOKEN_VERSION_MAJOR 1
#define TOKEN_VERSION_MINOR 0
#define DEFAULT_TPM2_SIZE 64
#define DEFAULT_PCR_BANK "sha256"
#define TPMS_NO_LIMIT	100
#define TPMS_MAX_DIGITS	2	// TPM no. 0-99
#define NV_NONCE_SIZE	32

#define NV_ARG		"plugin-tpm2-nv"
#define PCR_ARG		"plugin-tpm2-pcr"
#define BANK_ARG	"plugin-tpm2-bank"
#define DAPROTECT_ARG	"plugin-tpm2-daprotect"
#define NOPIN_ARG	"plugin-tpm2-no-pin"
#define TCTI_ARG	"plugin-tpm2-tcti"
#define FORCE_REMOVE_ARG	"plugin-tpm2-force-remove"

#define CREATE_VALID	(1 << 0)
#define CREATED		(1 << 1)
#define REMOVE_VALID	(1 << 2)
#define REMOVED		(1 << 3)

static void tpm2_token_dump(struct crypt_device *cd, const char *json)
{
	uint32_t nvindex, nonce_nvindex, pcrs, pcrbanks, version_major, version_minor;
	size_t nvkey_size;
	bool daprotect, pin;
	char* nv_nonce;
	char buf[1024], num[32];
	unsigned i, n;

	if (tpm2_token_read(cd, json, &version_minor, &version_major, &nvindex, &nonce_nvindex, &nv_nonce, &pcrs, &pcrbanks,
			    &daprotect, &pin, &nvkey_size)) {
		l_err(cd, "Cannot read JSON token metadata.");
		return;
	}

	l_std(cd, "\tTPM Token version:\t%" PRIx32 ".%" PRIx32 "\n", version_minor, version_major);
	l_std(cd, "\tPassphrase NVindex:\t0x%08" PRIx32 "\n", nvindex);
	l_std(cd, "\tPassphrase size:\t%zu [bytes]\n", nvkey_size);
	l_std(cd, "\tIdentification nonce NVindex:\t0x%08" PRIx32 "\n", nonce_nvindex);
	//l_std(cd, "\tIdentification NV Nonce:\t'%s' [%d bytes]\n", nv_nonce, NV_NONCE_SIZE);
	l_std(cd, "\tIdentification NV Nonce:\t");

	for (i = 0; (i + 1) < strlen(nv_nonce); i += 2) {
		if (i && i % 32 == 0)
			l_std(cd, "\n\t                        \t");
		l_std(cd, "%s%c%c", i % 32 == 0 ? "" : " ", nv_nonce[i], nv_nonce[i+1]);
	}
	l_std(cd, "\n");

	for (*buf = '\0', n = 0, i = 0; i < 32; i++) {
		if (!(pcrs & (1 << i)))
			continue;
		snprintf(num, sizeof(num), "%s%u", n ? "," : "", i);
		strcat(buf, num);
		n++;
	}
	l_std(cd, "\tPCRs:     %s\n", buf);

	*buf = '\0';
	n = 0;

	for (i = 0; i < CRYPT_HASH_ALGS_COUNT; i++) {
		if (pcrbanks & hash_algs[i].crypt_id) {
			if (n)
				strcat(buf, ",");
			strcat(buf, hash_algs[i].name);
			n++;
		}
	}

	l_std(cd, "\tPCRBanks: %s\n", buf);

	*buf = '\0';
	n = 0;
	if (daprotect) {
		strcat(buf, "DA_PROTECT");
		n++;
	}
	if (pin)
		strcat(buf, n++ ? ",PIN" : "PIN");
	l_std(cd, "\tflags:    %s\n", buf);

	free(nv_nonce);
}

static bool tpm2_verify_tcti_for_token(struct crypt_device *cd,
	int token,
	const char *tcti_spec)
{
	int r;
	bool is_valid = true;
	const char *json;
	uint32_t nvindex, nonce_nvindex;
	char *nv_nonce_str = NULL;
	char *nv_nonce_from_tpm = NULL;
	char *nv_nonce_from_tpm_str = NULL;

	TSS2_RC tpm_rc;
	ESYS_CONTEXT *ctx;

	if (tpm_init(cd, &ctx, tcti_spec) != TSS2_RC_SUCCESS)
		return false;

	r = crypt_token_json_get(cd, token, &json);
	if (r < 0) {
		l_err(cd, "Cannot read JSON token metadata.");
		is_valid = false;
		goto out;
	}

	r = tpm2_token_read(cd, json, NULL, NULL, &nvindex, &nonce_nvindex, &nv_nonce_str, NULL, NULL,
			    NULL, NULL, NULL);
	if (r < 0 || !nv_nonce_str) {
		l_err(cd, "Cannot read JSON token metadata.");
		is_valid = false;
		goto out;
	}

	nv_nonce_from_tpm = calloc(1, NV_NONCE_SIZE);
	if (!nv_nonce_from_tpm) {
		is_valid = false;
		goto out;
	}

	tpm_rc = tpm_nv_read(cd, ctx, nonce_nvindex, NULL, 0, 0, CRYPT_TPM_PCRBANK_SHA1, nv_nonce_from_tpm, NV_NONCE_SIZE);
	if (tpm_rc != TPM2_RC_SUCCESS) {
		l_dbg(cd, "Failed to read NV nonce, this TPM doesn't seem to hold the passphrase.");
		LOG_TPM_ERR(cd, tpm_rc);
		is_valid = false;
		goto out;
	}

	nv_nonce_from_tpm_str = bytes_to_hex(nv_nonce_from_tpm, NV_NONCE_SIZE);

	if (strncmp(nv_nonce_from_tpm_str, nv_nonce_str, NV_NONCE_SIZE)) {
		l_dbg(cd, "Bad NV nonce content, this TPM doesn't hold the passphrase.");
		l_dbg(cd, "Nonce from header: '%s'", nv_nonce_str);
		l_dbg(cd, "TPM-stored nonce: '%s'", nv_nonce_from_tpm_str);
		is_valid = false;
		goto out;
	}
out:
	if (nv_nonce_str) {
		free(nv_nonce_str);
	}
	if (nv_nonce_from_tpm) {
		free(nv_nonce_from_tpm);
	}
	if (nv_nonce_from_tpm_str) {
		free(nv_nonce_from_tpm_str);
	}

	Esys_Finalize(&ctx);
	return is_valid;
}

static char *tpm2_find_tcti_for_token(struct crypt_device *cd,
	int token)
{
	int access_error, i;

	const size_t tcti_name_len = strlen("device:/dev/tpmrm") + TPMS_MAX_DIGITS + 1;
	char *tcti_conf = malloc(tcti_name_len);
	if (!tcti_conf)
		return NULL;

	strcpy(tcti_conf, "tabrmd");
	l_dbg(cd, "Verifying TCTI '%s' for token %d\n", tcti_conf, token);

	if (tpm2_verify_tcti_for_token(cd, token, tcti_conf))
		return tcti_conf;

	for (i = 0; i < TPMS_NO_LIMIT; i++) {
		snprintf(tcti_conf, tcti_name_len, "device:/dev/tpmrm%d", i);
		l_dbg(cd, "Checking TPM device: '%s'\n", tcti_conf + 7);
		access_error = access(tcti_conf + 7, R_OK | W_OK);
		if (access_error) {
			l_dbg(cd, "Device does not exist", tcti_conf);
			break;
		}
		l_dbg(cd, "Device exists, verifying TCTI '%s' for token %d\n", tcti_conf, token);

		if (tpm2_verify_tcti_for_token(cd, token, tcti_conf))
			return tcti_conf;
	}

	for (i = 0; i < TPMS_NO_LIMIT; i++) {
		snprintf(tcti_conf, tcti_name_len, "device:/dev/tpmrm%d", i);
		l_dbg(cd, "Checking TPM device: '%s'\n", tcti_conf + 7);
		access_error = access(tcti_conf + 7, R_OK | W_OK);
		if (access_error) {
			l_dbg(cd, "Device does not exist", tcti_conf);
			break;
		}
		l_dbg(cd, "Device exists, verifying TCTI '%s' for token %d\n", tcti_conf, token);

		if (tpm2_verify_tcti_for_token(cd, token, tcti_conf))
			return tcti_conf;
	}

	free(tcti_conf);
	return NULL;
}

static int tpm2_token_open_pin_with_tcti(struct crypt_device *cd,
	int token,
	const char *tpm_pass,
	char **buffer,
	size_t *buffer_len,
	void *usrptr,
	const char *tcti_spec)
{
	int r;
	TSS2_RC tpm_rc;
	ESYS_CONTEXT *ctx;
	uint32_t nvindex, pcrselection, pcrbanks;
	size_t nvkey_size;
	bool daprotect, pin;
	const char *json;

	if (!tpm2_verify_tcti_for_token(cd, token, tcti_spec))
		return -EINVAL;

	if (tpm_init(cd, &ctx, tcti_spec) != TSS2_RC_SUCCESS)
		return -EACCES;

	r = crypt_token_json_get(cd, token, &json);
	if (r < 0) {
		l_err(cd, "Cannot read JSON token metadata.");
		goto out;
	}

	r = tpm2_token_read(cd, json, NULL, NULL, &nvindex, NULL, NULL, &pcrselection, &pcrbanks,
			    &daprotect, &pin, &nvkey_size);
	if (r < 0) {
		l_err(cd, "Cannot read JSON token metadata.");
		goto out;
	}

	if (pin && !tpm_pass) {
		if (daprotect)
			l_std(cd, "TPM stored password has dictionary attack protection turned on. "
				  "Don't enter password too many times.\n");
		r = -EAGAIN;
		goto out;
	}

	*buffer = malloc(nvkey_size);
	if (!(*buffer)) {
		 r = -ENOMEM;
		 goto out;
	}
	*buffer_len = nvkey_size;

	r = -EACCES;

	tpm_rc = tpm_nv_read(cd, ctx, nvindex, tpm_pass, tpm_pass ? strlen(tpm_pass) : 0,
			   pcrselection, pcrbanks, *buffer, nvkey_size);

	if (tpm_rc == TSS2_RC_SUCCESS) {
		r = 0;
	} else if (tpm_rc == (TPM2_RC_S | TPM2_RC_1 | TPM2_RC_BAD_AUTH) ||
	           tpm_rc == (TPM2_RC_S | TPM2_RC_1 | TPM2_RC_AUTH_FAIL)) {
		l_err(cd, "Failed to read passphrase NV index.");
		LOG_TPM_ERR(cd, tpm_rc);
		r = -EPERM;
	}

out:
	Esys_Finalize(&ctx);
	return r;
}

static int tpm2_token_open_pin(struct crypt_device *cd,
	int token,
	const char *tpm_pass,
	char **buffer,
	size_t *buffer_len,
	void *usrptr)
{
	char *tcti_conf;

	tcti_conf = tpm2_find_tcti_for_token(cd, token);

	if (!tcti_conf) {
		l_err(cd, "Couldn't find a TPM device associated with the TPM token.");
		return -EINVAL;
	}

	return tpm2_token_open_pin_with_tcti(cd, token, tpm_pass, buffer, buffer_len,usrptr, tcti_conf);
}

static int tpm2_token_open(struct crypt_device *cd,
	int token,
	char **buffer,
	size_t *buffer_len,
	void *usrptr)
{
	return tpm2_token_open_pin(cd, token, NULL, buffer, buffer_len, usrptr);
}

static int _tpm2_token_validate(struct crypt_device *cd, const char *json)
{
	return tpm2_token_validate(json);
}

struct tpm2_context {
	const char *tpmbanks_str;
	const char *tcti_str;
	uint32_t tpmbanks;
	uint32_t tpmnv;
	uint32_t tpmnonce_nv;
	uint32_t tpmpcrs;
	uint32_t pass_size;
	ESYS_CONTEXT *ctx;

	bool tpmdaprotect;
	bool no_tpm_pin;
	bool force_remove;

	int timeout;
	int keyslot;
	int token;

	uint8_t status;

	struct crypt_cli *cli;
};

const crypt_token_handler cryptsetup_token_handler = {
	.name  = "tpm2",
	.open  = tpm2_token_open,
	.open_pin = tpm2_token_open_pin,
	.validate = _tpm2_token_validate,
	.dump = tpm2_token_dump
};

int crypt_token_handle_init(struct crypt_cli *cli, void **handle)
{
	int r;
	struct tpm2_context *tc;

	if (!handle)
		return -EINVAL;

	tc = calloc(1, sizeof(*tc));
	if (!tc)
		return -ENOMEM;

	r = tpm2_token_get_pcrbanks(DEFAULT_PCR_BANK, &tc->tpmbanks);
	if (r < 0) {
		free(tc);
		return r;
	}

	tc->cli = cli;

	*handle = tc;

	return 0;
}

void crypt_token_handle_free(void *handle)
{
	free(handle);
}

#define VERSION_STR(A,B) \
do { \
	return #A "." #B; \
} while (0)

const char *crypt_token_version(void)
{
	VERSION_STR(TOKEN_VERSION_MAJOR, TOKEN_VERSION_MINOR);
}

static const crypt_arg_list create_args[] = {
	/* plugin specific args */
	{ NV_ARG,	"Select TPM's NV index",                   CRYPT_ARG_UINT32, &create_args[1] },
	{ PCR_ARG,	"Selection of TPM PCRs",                   CRYPT_ARG_UINT32, &create_args[2] },
	{ BANK_ARG,	"Selection of TPM PCR banks", 		   CRYPT_ARG_STRING, &create_args[3] },
	{ DAPROTECT_ARG,"Enable TPM dictionary attack protection", CRYPT_ARG_BOOL,   &create_args[4] },
	{ NOPIN_ARG,	"Don't PIN protect TPM NV index",          CRYPT_ARG_BOOL,   &create_args[5] },
	{ TCTI_ARG,	"Select TCTI in format <tcti>:<tcti arg>, e.g. device:/dev/tpm0",               CRYPT_ARG_STRING, &create_args[6] },
	/* inherited from cryptsetup core args */
	{ "key-size",	NULL,                                      CRYPT_ARG_UINT32, &create_args[7] },
	{ "token-id",	NULL,                                      CRYPT_ARG_INT32,  &create_args[8] },
	{ "key-slot",	NULL,                                      CRYPT_ARG_INT32,  &create_args[9] },
	{ "timeout",	NULL,                                      CRYPT_ARG_UINT32, NULL }
};

static const crypt_arg_list remove_args[] = {
	/* plugin specific args */
	{ NV_ARG,	"Select TPM's NV index",                   CRYPT_ARG_UINT32, &remove_args[1] },
	{ TCTI_ARG,	"Select TCTI in format <tcti>:<tcti arg>, e.g. device:/dev/tpm0",               CRYPT_ARG_STRING, &remove_args[2] },
	{ FORCE_REMOVE_ARG,	"Force remove the TPM token metadata from LUKS header, even if the TPM device is not present.", CRYPT_ARG_BOOL,  &remove_args[3] },
	/* inherited from cryptsetup core args */
	{ "token-id",	"Token number to remove",                                      CRYPT_ARG_INT32, NULL },
};

const crypt_arg_list *crypt_token_create_params(void)
{
	return create_args;
}

const crypt_arg_list *crypt_token_remove_params(void)
{
	return remove_args;
}

static int plugin_get_arg_value(struct crypt_device *cd, struct crypt_cli *cli, const char *key, crypt_arg_type_info type, void *rvalue)
{
	int r;
	crypt_arg_type_info ti;

	r = crypt_cli_arg_type(cli, key, &ti);
	if (r == -ENOENT)
		l_err(cd, "%s argument is not defined.", key);
	if (r)
		return r;

	if (ti != type) {
		l_err(cd, "%s argument type is unexpected.", key);
		return -EINVAL;
	}

	r = crypt_cli_arg_value(cli, key, rvalue);
	if (r)
		l_err(cd, "Failed to get %s value.", key);

	return r;
}

static int get_create_cli_args(struct crypt_device *cd, struct tpm2_context *tc)
{
	int r;

	r = plugin_get_arg_value(cd, tc->cli, "key-slot", CRYPT_ARG_INT32, &tc->keyslot);
	if (r)
		return r;

	r = plugin_get_arg_value(cd, tc->cli, "token-id", CRYPT_ARG_INT32, &tc->token);
	if (r)
		return r;

	if (crypt_cli_arg_set(tc->cli, "key-size")) {
		r = plugin_get_arg_value(cd, tc->cli, "key-size", CRYPT_ARG_UINT32, &tc->pass_size);
		if (r)
			return r;
	} else
		tc->pass_size = DEFAULT_TPM2_SIZE;

	r = plugin_get_arg_value(cd, tc->cli, "timeout", CRYPT_ARG_UINT32, &tc->timeout);
	if (r)
		return r;

	if (crypt_cli_arg_set(tc->cli, NV_ARG)) {
		r = plugin_get_arg_value(cd, tc->cli, NV_ARG, CRYPT_ARG_UINT32, &tc->tpmnv);
		if (r)
			return r;
	}

	if (crypt_cli_arg_set(tc->cli, PCR_ARG)) {
		r = plugin_get_arg_value(cd, tc->cli, PCR_ARG, CRYPT_ARG_UINT32, &tc->tpmpcrs);
		if (r)
			return r;
	}

	if (crypt_cli_arg_set(tc->cli, BANK_ARG)) {
		r = plugin_get_arg_value(cd, tc->cli, BANK_ARG, CRYPT_ARG_STRING, &tc->tpmbanks_str);
		if (r)
			return r;
	}

	if (crypt_cli_arg_set(tc->cli, TCTI_ARG)) {
		r = plugin_get_arg_value(cd, tc->cli, TCTI_ARG, CRYPT_ARG_STRING, &tc->tcti_str);
		if (r)
			return r;
	}

	tc->tpmdaprotect = crypt_cli_arg_set(tc->cli, DAPROTECT_ARG);
	tc->no_tpm_pin = crypt_cli_arg_set(tc->cli, NOPIN_ARG);

	return 0;
}

int crypt_token_validate_create_params(struct crypt_device *cd, void *handle)
{
	int r;
	struct tpm2_context *tc = (struct tpm2_context *)handle;

	if (!tc)
		return -EINVAL;

	r = get_create_cli_args(cd, tc);
	if (r)
		return r;

	if (tpm2_token_get_pcrbanks(tc->tpmbanks_str ?: DEFAULT_PCR_BANK, &tc->tpmbanks)) {
		l_err(cd, "Wrong PCR bank value.");
		return -EINVAL;
	}

	if (!tc->tpmbanks) {
		l_err(cd, "PCR banks must be selected.");
		return -EINVAL;
	}

	tc->status |= CREATE_VALID;

	return 0;
}

int crypt_token_create(struct crypt_device *cd, void *handle)
{
	char *existing_pass = NULL, *tpm_pin = NULL;
	char *random_pass = NULL;
	char *nv_nonce = NULL;
	char *nv_nonce_str = NULL;
	size_t existing_pass_len, tpm_pin_len = 0;
	int r;
	bool supports_algs_for_pcrs;
	TSS2_RC tpm_rc;
	struct tpm2_context *tc = (struct tpm2_context *)handle;

	if (!tc)
		return -EINVAL;

	if (!tc->status) {
		r = crypt_token_validate_create_params(cd, handle);
		if (r)
			return r;
	}

	if (tc->status != CREATE_VALID)
		return -EINVAL;

	if (tc->tcti_str)
		l_dbg(cd, "Initializing Esys with TCTI %s", tc->tcti_str);
	else
		l_dbg(cd, "Initializing Esys with default TCTI");

	if (tpm_init(cd, &tc->ctx, tc->tcti_str) != TSS2_RC_SUCCESS)
		return -EINVAL;

	tpm_rc = tpm2_supports_algs_for_pcrs(cd, tc->ctx, tc->tpmbanks, tc->tpmpcrs, &supports_algs_for_pcrs);
	if (tpm_rc != TSS2_RC_SUCCESS) {
		l_err(NULL, "Failed to get PCRS capability from TPM.");
		LOG_TPM_ERR(NULL, tpm_rc);
		r = -ECOMM;
		goto out;
	}

	if (!supports_algs_for_pcrs) {
		l_err(NULL, "Your TPM doesn't support selected PCR and banks combination.");
		r = -ENOTSUP;
		goto out;
	}

	random_pass = crypt_safe_alloc(tc->pass_size);
	if (!random_pass) {
		r = -ENOMEM;
		goto out;
	}

	r = tpm_get_random(cd, tc->ctx, random_pass, tc->pass_size);
	if (r < 0) {
		l_err(cd, "Failed to retrieve random data for the TPM keyslot from the TPM.");
		goto out;
	}

	nv_nonce = malloc(NV_NONCE_SIZE);
	if (!nv_nonce) {
		r = -ENOMEM;
		goto out;
	}

	r = tpm_get_random(cd, tc->ctx, nv_nonce, NV_NONCE_SIZE);
	if (r < 0) {
		l_err(cd, "Failed to retrieve random data for the TPM NV nonce from the TPM.");
		goto out;
	}

	nv_nonce_str = bytes_to_hex(nv_nonce, NV_NONCE_SIZE);

	r = crypt_cli_get_key("Enter existing LUKS2 pasphrase:",
			  &existing_pass, &existing_pass_len,
			  0, 0, NULL, tc->timeout, 0, 0, cd, NULL);
	if (r < 0)
		goto out;

	if (!tc->no_tpm_pin) {
		r = crypt_cli_get_key("Enter new TPM password:",
				  &tpm_pin, &tpm_pin_len,
				  0, 0, NULL, tc->timeout, 1, 0, cd, NULL);
		if (r < 0)
			goto out;
	}

	r = tpm_nv_find_and_write(cd, tc->ctx,  &tc->tpmnv, random_pass, tc->pass_size, tpm_pin, tpm_pin_len, tc->tpmbanks, tc->tpmpcrs, tc->tpmdaprotect);
	if (r < 0) {
		l_err(cd, "Failed to write passphrase to an NV index.");
		goto out;
	}


	r = tpm_nv_find_and_write(cd, tc->ctx,  &tc->tpmnonce_nv, nv_nonce, NV_NONCE_SIZE, NULL, 0,CRYPT_TPM_PCRBANK_SHA1, 0, false);
	if (r < 0) {
		l_err(cd, "Failed to write random identification nonce to an NV index.");
		goto err_pass_nv_defined;
	}

	r = crypt_keyslot_add_by_passphrase(cd, tc->keyslot, existing_pass, existing_pass_len, random_pass, tc->pass_size);
	if (r < 0) {
		if (r == -EPERM)
			l_err(cd, "Wrong LUKS2 passphrase supplied.");
		goto err_nonce_nv_defined;
	}
	tc->keyslot = r;
	l_std(cd, "Using keyslot %d.\n", tc->keyslot);

	r = tpm2_token_add(cd, tc->token, TOKEN_VERSION_MAJOR, TOKEN_VERSION_MINOR, tc->tpmnv, tc->tpmnonce_nv, nv_nonce_str, tc->tpmpcrs, tc->tpmbanks, tc->tpmdaprotect, !tc->no_tpm_pin, tc->pass_size);
	if (r < 0) {
		goto err_keyslot_created;
	}
	tc->token = r;
	l_std(cd, "Token: %d\n", tc->token);

	r = crypt_token_assign_keyslot(cd, tc->token, tc->keyslot);
	if (r < 0) {
		l_err(cd, "Failed to assign keyslot %d to token %d.", tc->keyslot, tc->token);
		crypt_token_json_set(cd, tc->token, NULL);
		goto err_keyslot_created;
	}

	if (r > 0) {
		r = 0;
		tc->status |= CREATED;
	}
	goto out;

err_keyslot_created:
	crypt_keyslot_destroy(cd, tc->keyslot);
err_nonce_nv_defined:
	tpm_nv_undefine(cd, tc->ctx, tc->tpmnonce_nv);
	tc->tpmnonce_nv = 0;
err_pass_nv_defined:
	tpm_nv_undefine(cd, tc->ctx, tc->tpmnv);
	tc->tpmnv = 0;
out:
	if(nv_nonce)
		free(nv_nonce);
	if (random_pass)
		crypt_safe_free(random_pass);
	if (existing_pass)
		crypt_safe_free(existing_pass);
	if (tpm_pin)
		crypt_safe_free(tpm_pin);

	Esys_Finalize(&tc->ctx);
	return r;
}

static int get_remove_cli_args(struct crypt_device *cd, struct tpm2_context *tc)
{
	int r;

	tc->force_remove = crypt_cli_arg_set(tc->cli, FORCE_REMOVE_ARG);

	r = plugin_get_arg_value(cd, tc->cli, "token-id", CRYPT_ARG_INT32, &tc->token);
	if (r)
		return r;

	if (crypt_cli_arg_set(tc->cli, NV_ARG)) {
		r = plugin_get_arg_value(cd, tc->cli, NV_ARG, CRYPT_ARG_UINT32, &tc->tpmnv);
		if (r)
			return r;
	}

	if (crypt_cli_arg_set(tc->cli, TCTI_ARG)) {
		r = plugin_get_arg_value(cd, tc->cli, TCTI_ARG, CRYPT_ARG_STRING, &tc->tcti_str);
		if (r)
			return r;
	}


	return 0;
}

int crypt_token_validate_remove_params(struct crypt_device *cd, void *handle)
{
	int r;
	struct tpm2_context *tc = (struct tpm2_context *)handle;

	if (!tc || tc->status)
		return -EINVAL;

	r = get_remove_cli_args(cd, tc);
	if (r)
		return r;

	if (tc->token < 0 && tc->token != CRYPT_ANY_TOKEN) {
		l_err(cd, "Invalid token specification.");
		return -EINVAL;
	}

	if (!tc->tpmnv && tc->token == CRYPT_ANY_TOKEN) {
		l_err(cd, "Token ID or TPM2 nvindex option must be specified.");
		return -EINVAL;
	}

	tc->status = REMOVE_VALID;

	return 0;
}

int crypt_token_remove(struct crypt_device *cd, void *handle)
{
	int i, r;
	const char *type;
	char *found_tcti_conf = NULL;
	struct tpm2_context *tc = (struct tpm2_context *)handle;

	if (!tc)
		return -EINVAL;

	if (!tc->status) {
		r = crypt_token_validate_remove_params(cd, handle);
		if (r)
			return r;
	}

	if (tc->status != REMOVE_VALID)
		return -EINVAL;

	if (tc->token == CRYPT_ANY_TOKEN)
		tc->token = tpm2_token_by_nvindex(cd, tc->tpmnv);

	if (tc->token < 0 ||
	    crypt_token_status(cd, tc->token, &type) != CRYPT_TOKEN_EXTERNAL ||
	    strcmp(type, "tpm2")) {
		l_err(cd, "No TPM2 token to destroy.");
		return -EINVAL;
	}

	if (tc->tcti_str && !tpm2_verify_tcti_for_token(cd, tc->token, tc->tcti_str) && !tc->force_remove) {
		l_err(cd, "TPM device accessed via specified TCTI '%s' is not associated to this TPM token.", tc->tcti_str);
		return -EINVAL;
	}

	if (!tc->tcti_str) {
		l_dbg(cd, "No TCTI was specified, scanning...");
		found_tcti_conf = tpm2_find_tcti_for_token(cd, tc->token);

		if (!found_tcti_conf && !tc->force_remove) {
			l_err(cd, "No TPM device associated to this TPM token was found.");
			return -EINVAL;
		}
	}

	/* Destroy all keyslots assigned to TPM 2 token */
	for (i = 0; i < crypt_keyslot_max(CRYPT_LUKS2); i++) {
		if (!crypt_token_is_assigned(cd, tc->token, i)) {
			r = crypt_keyslot_destroy(cd, i);
			if (r < 0) {
				l_err(cd, "Cannot destroy keyslot %d.", i);
				if (found_tcti_conf)
					free(found_tcti_conf);
				return r;
			}
		}
	}

	if (tpm_init(cd, &tc->ctx, tc->tcti_str ? tc->tcti_str : found_tcti_conf) != TSS2_RC_SUCCESS) {
		if (found_tcti_conf)
			free(found_tcti_conf);
		return -EINVAL;
	}

	/* Destroy TPM2 NV index and token object itself */
	r = tpm2_token_kill(cd, tc->ctx, tc->token);
	if (!r)
		tc->status |= REMOVED;

	Esys_Finalize(&tc->ctx);

	if (found_tcti_conf)
		free(found_tcti_conf);
	return r;
}
