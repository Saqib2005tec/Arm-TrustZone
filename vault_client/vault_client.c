// SPDX-License-Identifier: BSD-2-Clause
/*
 * Key Vault Client Application
 * Running in Normal World Linux User Space (EL0)
 * Uses the GlobalPlatform TEE Client API (libteec) to communicate with OP-TEE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tee_client_api.h>
#include "../key_vault_ta/key_vault_ta.h"

int main(int argc, char *argv[])
{
    TEEC_Result res;
    TEEC_Context ctx;
    TEEC_Session sess;
    TEEC_Operation op;
    TEEC_UUID uuid = TA_KEY_VAULT_UUID;
    uint32_t err_origin;

    printf("\n=======================================================\n");
    printf("   ARM TrustZone Key Vault Client (Normal World EL0)  \n");
    printf("=======================================================\n");

    /* 1. Initialize Context with OP-TEE driver (/dev/tee0) */
    res = TEEC_InitializeContext(NULL, &ctx);
    if (res != TEEC_SUCCESS) {
        printf("[-] Failed to initialize TEE context: 0x%08x\n", res);
        return 1;
    }
    printf("[+] Successfully connected to /dev/tee0\n");

    /* 2. Open Session to Key Vault TA */
    res = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin);
    if (res != TEEC_SUCCESS) {
        printf("[-] Failed to open session to Key Vault TA: 0x%08x (origin=0x%x)\n", res, err_origin);
        TEEC_FinalizeContext(&ctx);
        return 1;
    }
    printf("[+] Secure Session OPENED with Key Vault TA in Secure EL0!\n");

    /* 3. Query Vault Info */
    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);

    res = TEEC_InvokeCommand(&sess, TA_KEY_VAULT_CMD_GET_INFO, &op, &err_origin);
    if (res == TEEC_SUCCESS) {
        printf("[+] Key Vault Info: KeySize=%u bits, Status=0x%X\n", op.params[0].value.a, op.params[0].value.b);
    }

    /* 4. Encrypt Plaintext Message */
    char plaintext[128] = "TOP_SECRET: My password is SuperSecretPassword123!";
    if (argc > 1) {
        strncpy(plaintext, argv[1], sizeof(plaintext) - 1);
        plaintext[sizeof(plaintext) - 1] = '\0';
    }
    size_t len = strlen(plaintext);
    char ciphertext[128] = {0};
    char decrypted[128] = {0};

    printf("\n[Original Plaintext] : \"%s\" (len=%zu)\n", plaintext, len);

    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, TEEC_MEMREF_TEMP_OUTPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].tmpref.buffer = plaintext;
    op.params[0].tmpref.size = len;
    op.params[1].tmpref.buffer = ciphertext;
    op.params[1].tmpref.size = sizeof(ciphertext);

    res = TEEC_InvokeCommand(&sess, TA_KEY_VAULT_CMD_ENCRYPT, &op, &err_origin);
    if (res != TEEC_SUCCESS) {
        printf("[-] Encryption failed: 0x%08x\n", res);
    } else {
        printf("[+] Encrypted via S-EL0 Key Vault! Ciphertext bytes:\n    ");
        for (size_t i = 0; i < len; i++) {
            printf("%02X ", (uint8_t)ciphertext[i]);
        }
        printf("\n");
    }

    /* 5. Decrypt Back to verify correctness */
    memset(&op, 0, sizeof(op));
    op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, TEEC_MEMREF_TEMP_OUTPUT, TEEC_NONE, TEEC_NONE);
    op.params[0].tmpref.buffer = ciphertext;
    op.params[0].tmpref.size = len;
    op.params[1].tmpref.buffer = decrypted;
    op.params[1].tmpref.size = sizeof(decrypted);

    res = TEEC_InvokeCommand(&sess, TA_KEY_VAULT_CMD_DECRYPT, &op, &err_origin);
    if (res == TEEC_SUCCESS) {
        decrypted[len] = '\0';
        printf("[+] Decrypted via S-EL0 Key Vault: \"%s\"\n", decrypted);
        if (strcmp(plaintext, decrypted) == 0) {
            printf("\n[SUCCESS] Cryptographic loop verified! Secret key was used without Linux ever reading it!\n");
        }
    }

    /* 6. Clean up */
    TEEC_CloseSession(&sess);
    TEEC_FinalizeContext(&ctx);
    printf("=======================================================\n\n");
    return 0;
}

