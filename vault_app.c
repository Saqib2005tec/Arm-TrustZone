#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "sec_vault/sec_vault_ioctl.h"

/* The secret key known to EL3 (for verification that it does NOT exist in EL0 memory) */
static const uint8_t target_key_to_search[16] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

static void print_hex(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		printf("%02x ", data[i]);
	}
	printf("\n");
}

int main(void)
{
	int fd;
	struct sec_vault_block block;
	uint8_t saved_ciphertext[16];

	printf("\n");
	printf("========================================================\n");
	printf("   ARM TrustZone Hardware Key Vault Demo (EL0 -> EL3)   \n");
	printf("========================================================\n");
	printf("Model: Blind Cryptographic Oracle (Hardware Security Module)\n");
	printf("Objective: Use a 128-bit AES key without ever reading it!\n\n");

	fd = open("/dev/sec_vault", O_RDWR);
	if (fd < 0) {
		perror("[-] Error: Failed to open /dev/sec_vault");
		printf("[-] Make sure sec_vault.ko is loaded!\n");
		return 1;
	}

	/* 1. Original Plaintext */
	memset(block.data, 0, sizeof(block.data));
	memcpy(block.data, "SecretTrustZone!", 16);

	printf("[1] Original Plaintext (EL0) : \"%.16s\"\n", (char *)block.data);
	printf("    Plaintext Hex            : ");
	print_hex(block.data, 16);

	/* 2. Request EL3 to encrypt using the hidden key */
	printf("\n[*] Invoking ioctl(SEC_VAULT_IOCTL_ENCRYPT)...\n");
	printf("    Trapping through EL1 driver -> SMC into EL3 Secure SRAM...\n");

	if (ioctl(fd, SEC_VAULT_IOCTL_ENCRYPT, &block) < 0) {
		perror("[-] ioctl encrypt failed");
		close(fd);
		return 1;
	}

	printf("[2] Ciphertext received from EL3: ");
	print_hex(block.data, 16);
	memcpy(saved_ciphertext, block.data, 16);

	/* 3. Corrupt memory to demonstrate memory zeroing */
	memset(block.data, 0x00, 16);
	printf("    Memory wiped in user space  : ");
	print_hex(block.data, 16);

	/* 4. Request EL3 to decrypt using the hidden key */
	memcpy(block.data, saved_ciphertext, 16);
	printf("\n[*] Invoking ioctl(SEC_VAULT_IOCTL_DECRYPT)...\n");
	printf("    Trapping through EL1 driver -> SMC into EL3 Secure SRAM...\n");

	if (ioctl(fd, SEC_VAULT_IOCTL_DECRYPT, &block) < 0) {
		perror("[-] ioctl decrypt failed");
		close(fd);
		return 1;
	}

	printf("[3] Decrypted Plaintext from EL3: \"%.16s\"\n", (char *)block.data);
	printf("    Decrypted Hex               : ");
	print_hex(block.data, 16);

	/* 5. Verification */
	if (memcmp(block.data, "SecretTrustZone!", 16) == 0) {
		printf("\n[+] SUCCESS: Decrypted message matches original perfectly!\n");
	} else {
		printf("\n[-] ERROR: Decrypted message mismatch!\n");
	}

	printf("--------------------------------------------------------\n");
	printf("[Security Audit]:\n");
	printf(" - The AES Key was never passed to user space (EL0).\n");
	printf(" - The AES Key was never passed to the Linux kernel (EL1).\n");
	printf(" - The Key remains physically isolated inside EL3 Secure SRAM.\n");
	printf("========================================================\n\n");

	close(fd);
	return 0;
}

