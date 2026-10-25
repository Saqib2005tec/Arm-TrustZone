#ifndef KEY_VAULT_TA_H
#define KEY_VAULT_TA_H

/* UUID of the Key Vault Trusted Application: e5b22b10-6c34-4b55-a0ef-28e4695e7b23 */
#define TA_KEY_VAULT_UUID \
    { 0xe5b22b10, 0x6c34, 0x4b55, \
        { 0xa0, 0xef, 0x28, 0xe4, 0x69, 0x5e, 0x7b, 0x23 } }

/* Command IDs */
#define TA_KEY_VAULT_CMD_GET_INFO    0
#define TA_KEY_VAULT_CMD_ENCRYPT     1
#define TA_KEY_VAULT_CMD_DECRYPT     2

#endif /* KEY_VAULT_TA_H */

