// SPDX-License-Identifier: BSD-2-Clause
/*
 * Key Vault Trusted Application (TA)
 * Running inside ARM TrustZone Secure EL0 (User Space)
 */

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <trace.h>
#include "key_vault_ta.h"

/*
 * =========================================================================
 * THE SECRET CRYPTOGRAPHIC KEY
 * =========================================================================
 * This 128-bit key is compiled directly into the Trusted Application.
 * When this TA executes, this memory address sits in SECURE RAM (S-EL0).
 * Even if the Linux kernel is completely taken over by root/hackers,
 * the hardware memory controller (TZASC) prevents them from reading this!
 */
static const uint8_t secret_vault_key[16] = {
    0x4B, 0x45, 0x59, 0x5F, 0x56, 0x41, 0x55, 0x4C,
    0x54, 0x5F, 0x53, 0x45, 0x43, 0x52, 0x45, 0x54  /* "KEY_VAULT_SECRET" */
};

/*
 * Entry Points required by the GlobalPlatform TEE Internal Core API
 */

TEE_Result TA_CreateEntryPoint(void)
{
    IMSG("====================================================");
    IMSG("[KeyVault-TA] Created in Secure EL0!");
    IMSG("[KeyVault-TA] 128-bit Secret Key loaded into Secure RAM.");
    IMSG("====================================================");
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
    IMSG("[KeyVault-TA] Destroyed.");
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types,
                                    TEE_Param params[4],
                                    void **sess_ctx)
{
    (void)param_types;
    (void)params;
    (void)sess_ctx;

    IMSG("[KeyVault-TA] Normal World (Linux) opened a secure session.");
    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *sess_ctx)
{
    (void)sess_ctx;
    IMSG("[KeyVault-TA] Session closed.");
}

/*
 * Internal Cryptographic Stream Cipher Implementation
 * Computes: out[i] = in[i] ^ key[i % 16] ^ ((i * 13 + 0x5A) & 0xFF)
 */
static void vault_cipher(const uint8_t *in, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t keystream = secret_vault_key[i % sizeof(secret_vault_key)] ^ (uint8_t)((i * 13 + 0x5A) & 0xFF);
        out[i] = in[i] ^ keystream;
    }
}

/*
 * The Workhorse: Called when Linux invokes a command
 */
TEE_Result TA_InvokeCommandEntryPoint(void *sess_ctx,
                                      uint32_t cmd_id,
                                      uint32_t param_types,
                                      TEE_Param params[4])
{
    (void)sess_ctx;

    switch (cmd_id) {
    case TA_KEY_VAULT_CMD_GET_INFO: {
        /*
         * Expected parameters:
         * params[0]: Output Value (a = Key Size in bits, b = Status Code)
         */
        uint32_t exp_param_types = TEE_PARAM_TYPES(
            TEE_PARAM_TYPE_VALUE_OUTPUT,
            TEE_PARAM_TYPE_NONE,
            TEE_PARAM_TYPE_NONE,
            TEE_PARAM_TYPE_NONE);

        if (param_types != exp_param_types) {
            EMSG("[KeyVault-TA] Bad parameters for GET_INFO: 0x%x", param_types);
            return TEE_ERROR_BAD_PARAMETERS;
        }

        params[0].value.a = 128;      /* 128-bit key */
        params[0].value.b = 0xCAFE;   /* Status: Vault is Online and Locked */

        IMSG("[KeyVault-TA] CMD_GET_INFO: KeySize=128 bits, Status=0xCAFE");
        return TEE_SUCCESS;
    }

    case TA_KEY_VAULT_CMD_ENCRYPT:
    case TA_KEY_VAULT_CMD_DECRYPT: {
        /*
         * Expected parameters:
         * params[0]: Input Buffer (MEMREF_INPUT)
         * params[1]: Output Buffer (MEMREF_OUTPUT)
         */
        uint32_t exp_param_types = TEE_PARAM_TYPES(
            TEE_PARAM_TYPE_MEMREF_INPUT,
            TEE_PARAM_TYPE_MEMREF_OUTPUT,
            TEE_PARAM_TYPE_NONE,
            TEE_PARAM_TYPE_NONE);

        if (param_types != exp_param_types) {
            EMSG("[KeyVault-TA] Bad parameters for CRYPTO: 0x%x", param_types);
            return TEE_ERROR_BAD_PARAMETERS;
        }

        const uint8_t *in = (const uint8_t *)params[0].memref.buffer;
        size_t in_size = params[0].memref.size;
        uint8_t *out = (uint8_t *)params[1].memref.buffer;
        size_t out_size = params[1].memref.size;

        if (!in || !out)
            return TEE_ERROR_BAD_PARAMETERS;

        if (out_size < in_size) {
            params[1].memref.size = in_size;
            return TEE_ERROR_SHORT_BUFFER;
        }

        /* Perform cryptographic transformation using the secret key */
        vault_cipher(in, out, in_size);
        params[1].memref.size = in_size;

        if (cmd_id == TA_KEY_VAULT_CMD_ENCRYPT)
            IMSG("[KeyVault-TA] CMD_ENCRYPT: Processed %zu bytes with secret key.", in_size);
        else
            IMSG("[KeyVault-TA] CMD_DECRYPT: Processed %zu bytes with secret key.", in_size);

        return TEE_SUCCESS;
    }

    default:
        EMSG("[KeyVault-TA] Unknown Command ID: %u", cmd_id);
        return TEE_ERROR_NOT_SUPPORTED;
    }
}

