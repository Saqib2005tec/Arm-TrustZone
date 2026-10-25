# Exhaustive Technical & Architectural Reference: ARM64 TrustZone Hardware Key Vault System

---

## Executive Overview

This document provides a comprehensive, line-by-line, and file-by-file technical reference for the **ARM64 TrustZone Hardware Cryptographic Key Vault (HSM)** system.

The core objective of this system is to enable a user-space application running under a minimal embedded Linux OS to utilize a 128-bit AES cryptographic key **without the application, the operating system, or any privileged entity (including `root`) ever possessing, reading, or mapping the key into Normal World memory.**

The system spans all four ARMv8-A Exception Levels:
* **EL0 (User Space)**: Unprivileged application ([`vault_app`](file:///home/gg-gamer/embedde_gg/linux_gg/arm64_scratch/vault_app.c)).
* **EL1 (Kernel Space)**: Linux Kernel driver ([`sec_vault.ko`](file:///home/gg-gamer/embedde_gg/linux_gg/arm64_scratch/sec_vault/sec_vault.c)) and filesystem `/dev/sec_vault`.
* **EL2 (Hypervisor Space)**: Das U-Boot bootloader staging and dispatching the system.
* **EL3 (Secure Monitor Space)**: ARM Trusted Firmware (TF-A BL31) hosting the private key and the AES-128 engine in Secure SRAM.

---

```
======================================================================================================
                                     SYSTEM ARCHITECTURE MAP
======================================================================================================

 [ NORMAL WORLD ]                                                     [ SECURE WORLD ]
 
 ┌────────────────────────────────────────────────────────┐
 │ Exception Level 0 (EL0) - User Space                   │
 │   • Process: /bin/vault_app                            │
 │   • Memory: Isolated Virtual Memory (TTBR0_EL1)        │
 │   • Plaintext: "SecretTrustZone!"                      │
 └──────────────────────────┬─────────────────────────────┘
                            │ System Call: ioctl(fd, SEC_VAULT_IOCTL_ENCRYPT, &blk)
                            │ Hardware Trap: svc #0  (Supervisor Call)
 ┌──────────────────────────▼─────────────────────────────┐
 │ Exception Level 1 (EL1) - Linux Kernel Space           │
 │   • Driver: /dev/sec_vault (sec_vault.ko)              │
 │   • Memory: Kernel High Virtual Memory (TTBR1_EL1)     │
 │   • Packs 16 bytes into CPU registers X1 and X2        │
 └──────────────────────────┬─────────────────────────────┘
                            │ Architectural Boundary: arm_smccc_smc()
                            │ Hardware Trap: smc #0  (Secure Monitor Call)
                            │ CPU transitions Non-Secure -> Secure World
                            │                                         ┌───────────────────────────────┐
                            │                                         │ Exception Level 3 (EL3) - TF-A│
                            │                                         │ • Binary: BL31 Runtime Monitor│
                            │                                         │ • Memory: Secure SRAM (0x0E00)│
                            │                                         │ • Key: secret_vault_key (AES) │
                            └────────────────────────────────────────►│ • ven_el3_svc_handler()       │
                                                                      │   runs vault_aes128_encrypt() │
                                                                      │ • Writes Ciphertext to X0, X1 │
                                                                      │ • Executes: eret (Return)     │
                                                                      └───────────────┬───────────────┘
 ┌────────────────────────────────────────────────────────┐                           │
 │ Returns to EL1 (Kernel) -> Copies X0, X1 to user buffer◄───────────────────────────┘
 └──────────────────────────┬─────────────────────────────┘
                            │ Syscall returns to EL0
 ┌──────────────────────────▼─────────────────────────────┐
 │ User Space Application prints Ciphertext from EL3:     │
 │   2d 6c 2c e1 d3 eb cd c5 cb 4d bf 1d 4f f3 c6 07     │
 └────────────────────────────────────────────────────────┘
```

---

## SECTION 1: EL3 Firmware Layer (ARM Trusted Firmware)

### 1.1 Source File: `trusted-firmware-a/services/el3/ven_el3_svc.c`

This file implements the **Secure Monitor Runtime Service** running at Exception Level 3 (EL3). It holds the cryptographic key in Secure SRAM and executes cryptographic operations inside Secure World.

#### Detailed Code Breakdown

```c
/* vendor-specific EL3 UUID */
DEFINE_SVC_UUID2(ven_el3_svc_uid,
	0xb6011dca, 0x57c4, 0x407e, 0x83, 0xf0,
	0xa7, 0xed, 0xda, 0xf0, 0xdf, 0x6c);
```
* **Lines 19–22**: `DEFINE_SVC_UUID2` is a TF-A macro that declares a 128-bit Universally Unique Identifier (`uuid_t`) for this service: `b6011dca-57c4-407e-83f0-a7eddaf0df6c`.
* **Design Decision**: In the ARM SMC Calling Convention (SMCCC), every service must be queryable via a standard UID function (`0x8700ff01`). This ensures the operating system can confirm the firmware service is loaded before attempting cryptographic calls.

```c
#define VEN_EL3_SVC_VAULT_ENCRYPT_64	0xC7000040U
#define VEN_EL3_SVC_VAULT_ENCRYPT_32	0x87000040U
#define VEN_EL3_SVC_VAULT_DECRYPT_64	0xC7000041U
#define VEN_EL3_SVC_VAULT_DECRYPT_32	0x87000041U
```
* **Lines 31–34**: Defines the **SMC Function Identifiers (FIDs)** according to the ARM SMCCC specification:
  * Bit 31 (`1`): **Fast Call**. Must execute atomically without yielding the CPU or sleeping.
  * Bit 30 (`1` for `0xC7...`, `0` for `0x87...`): Calling convention. `1` specifies **SMC64** (arguments and returns are 64-bit registers), while `0` specifies **SMC32**.
  * Bits [29:24] (`0b000111` = `7`): **Owning Entity Number (OEN)**. OEN 7 designates *Vendor-Specific EL3 Monitor Service Calls*.
  * Bits [15:0] (`0x0040` / `0x0041`): The specific function within OEN 7 (`0x0040` = Encrypt, `0x0041` = Decrypt).
* **Design Decision**: Supporting both `0xC7000040` and `0x87000040` guarantees compatibility whether the caller operates in pure 64-bit register mode or uses legacy 32-bit register packing.

```c
static const uint8_t secret_vault_key[16] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};
```
* **Lines 37–40**: The **128-bit Secret Master Key**.
* **Memory Placement**: Because this variable is marked `static const` inside BL31, the linker script places it into the `.rodata` section of BL31.
* **Security Attribute**: In QEMU and real hardware, BL31 is linked to execute from physical memory region `0x0E000000` (**Trusted SRAM**). The hardware memory controller (TrustZone Address Space Controller - TZASC) tags this entire region as **Secure-Only**. If any instruction executed at EL1 (Linux) or EL0 attempts to read memory address `0x0E000000`, the hardware memory bus triggers an immediate **External Data Abort (Bus Error)**.

```c
static const uint8_t aes_sbox[256] = { ... };
static const uint8_t aes_rsbox[256] = { ... };
```
* **Lines 42–76**: The **Forward S-Box** and **Inverse S-Box (RS-Box)**.
* **Mathematical Rationale**: AES operates on the Galois Field $GF(2^8)$ modulo the irreducible polynomial $m(x) = x^8 + x^4 + x^3 + x + 1$ (`0x11B`). The S-Box performs a non-linear byte substitution: it takes the multiplicative inverse in $GF(2^8)$ followed by an affine transformation over $GF(2)$. The `aes_rsbox` reverses this transformation during decryption.
* **Design Decision**: Storing these as precomputed 256-byte lookup tables in ROM eliminates runtime calculation overhead and guarantees constant-time lookups resistant to timing attacks.

```c
static const uint8_t aes_rcon[11] = {
	0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};
```
* **Lines 78–80**: The **Round Constant (Rcon)** table. Used in AES Key Expansion to eliminate symmetry across key rounds. Each value represents $x^{i-1}$ in $GF(2^8)$.

```c
static uint8_t aes_xtime(uint8_t x)
{
	return ((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}
```
* **Lines 82–85**: Multiplication by 2 ($x \cdot 02$) in the field $GF(2^8)$. If the high bit of $x$ is 1 (overflow past 8 bits), it reduces modulo `0x1B`.

```c
static uint8_t aes_multiply(uint8_t x, uint8_t y)
{
	return (((y & 1) * x) ^
		((y >> 1 & 1) * aes_xtime(x)) ^
		((y >> 2 & 1) * aes_xtime(aes_xtime(x))) ^
		((y >> 3 & 1) * aes_xtime(aes_xtime(aes_xtime(x)))) ^
		((y >> 4 & 1) * aes_xtime(aes_xtime(aes_xtime(aes_xtime(x))))));
}
```
* **Lines 87–94**: Full Galois field multiplication of two elements in $GF(2^8)$. Used exclusively by `aes_inv_mix_columns` during decryption to multiply by the coefficients `0x0E`, `0x0B`, `0x0D`, and `0x09`.

```c
static void aes_key_expansion(uint8_t *round_key, const uint8_t *key)
{
	int i, j;
	uint8_t tempa[4];

	for (i = 0; i < 4; ++i) {
		round_key[(i * 4) + 0] = key[(i * 4) + 0];
		round_key[(i * 4) + 1] = key[(i * 4) + 1];
		round_key[(i * 4) + 2] = key[(i * 4) + 2];
		round_key[(i * 4) + 3] = key[(i * 4) + 3];
	}

	for (i = 4; i < 44; ++i) {
		j = (i - 1) * 4;
		tempa[0] = round_key[j + 0];
		tempa[1] = round_key[j + 1];
		tempa[2] = round_key[j + 2];
		tempa[3] = round_key[j + 3];

		if (i % 4 == 0) {
			uint8_t u8tmp = tempa[0];
			tempa[0] = aes_sbox[tempa[1]] ^ aes_rcon[i / 4];
			tempa[1] = aes_sbox[tempa[2]];
			tempa[2] = aes_sbox[tempa[3]];
			tempa[3] = aes_sbox[u8tmp];
		}

		j = i * 4;
		round_key[j + 0] = round_key[(i - 4) * 4 + 0] ^ tempa[0];
		round_key[j + 1] = round_key[(i - 4) * 4 + 1] ^ tempa[1];
		round_key[j + 2] = round_key[(i - 4) * 4 + 2] ^ tempa[2];
		round_key[j + 3] = round_key[(i - 4) * 4 + 3] ^ tempa[3];
	}
}
```
* **Lines 96–130**: Expands the 16-byte (128-bit) secret master key into **176 bytes of round keys** (11 rounds $\times$ 16 bytes = 44 words of 32 bits).
  * Every 4th word undergoes `RotWord` (cyclic permutation), `SubWord` (S-box lookup on 4 bytes), and XOR with `aes_rcon[i / 4]`.
  * The rest of the words are calculated by XORing the previous word with the word 4 positions earlier.

```c
static void aes_add_round_key(uint8_t round, uint8_t state[4][4], const uint8_t *round_key)
```
* **Lines 132–139**: Performs bitwise XOR between the 16-byte matrix `state` and the round key corresponding to `round * 16`.

```c
static void aes_sub_bytes(uint8_t state[4][4])
static void aes_inv_sub_bytes(uint8_t state[4][4])
```
* **Lines 141–155**: Replaces every byte in the $4 \times 4$ state array with its non-linear counterpart from `aes_sbox` (encryption) or `aes_rsbox` (decryption).

```c
static void aes_shift_rows(uint8_t state[4][4])
static void aes_inv_shift_rows(uint8_t state[4][4])
```
* **Lines 157–199**: Cyclically shifts the rows of the state array to provide diffusion:
  * Row 0: No shift.
  * Row 1: Shifted left by 1 byte (shift right by 1 for inverse).
  * Row 2: Shifted left by 2 bytes.
  * Row 3: Shifted left by 3 bytes.

```c
static void aes_mix_columns(uint8_t state[4][4])
static void aes_inv_mix_columns(uint8_t state[4][4])
```
* **Lines 201–222**: Treats each column of the $4 \times 4$ state as a four-term polynomial over $GF(2^8)$ and multiplies it modulo $x^4 + 1$ with the fixed polynomial $a(x) = \{03\}x^3 + \{01\}x^2 + \{01\}x + \{02\}$. This guarantees that each output byte depends on all four input bytes of that column.

```c
static void vault_aes128_encrypt(const uint8_t in[16], uint8_t out[16], const uint8_t key[16])
```
* **Lines 224–249**: Implements the **standard 10-round AES-128 encryption loop**:
  1. `KeyExpansion`: Derives round keys from `secret_vault_key`.
  2. `AddRoundKey(0)`: Initial round key whitening.
  3. **Rounds 1 through 9**: `SubBytes` $\rightarrow$ `ShiftRows` $\rightarrow$ `MixColumns` $\rightarrow$ `AddRoundKey(round)`.
  4. **Round 10 (Final Round)**: `SubBytes` $\rightarrow$ `ShiftRows` $\rightarrow$ `AddRoundKey(10)` (*MixColumns is omitted in the final round according to FIPS-197*).
  5. Copies the resulting state matrix into `out`.

```c
static void vault_aes128_decrypt(const uint8_t in[16], uint8_t out[16], const uint8_t key[16])
```
* **Lines 251–276**: Implements the **standard 10-round AES-128 decryption loop**:
  1. `AddRoundKey(10)`: Initial round key whitening with the 10th round key.
  2. **Rounds 9 down to 1**: `InvShiftRows` $\rightarrow$ `InvSubBytes` $\rightarrow$ `AddRoundKey(round)` $\rightarrow$ `InvMixColumns`.
  3. **Round 0 (Final Inverse Round)**: `InvShiftRows` $\rightarrow$ `InvSubBytes` $\rightarrow$ `AddRoundKey(0)`.

```c
static uintptr_t ven_el3_svc_handler(unsigned int smc_fid,
			u_register_t x1,
			u_register_t x2,
			u_register_t x3,
			u_register_t x4,
			void *cookie,
			void *handle,
			u_register_t flags)
```
* **Lines 304–313**: The central entry point called by TF-A whenever an SMC belonging to OEN 7 occurs.
  * `smc_fid`: Function ID from register `x0`.
  * `x1`–`x4`: General purpose argument registers passed by Linux at EL1.
  * `handle`: Pointer to the saved CPU context (`cpu_context_t`) representing the Non-Secure world registers.

```c
	switch (smc_fid) {
	case VEN_EL3_SVC_UID:
		SMC_UUID_RET(handle, ven_el3_svc_uid);
		break;
	case VEN_EL3_SVC_VERSION:
		SMC_RET2(handle, VEN_EL3_SVC_VERSION_MAJOR, VEN_EL3_SVC_VERSION_MINOR);
		break;
	case VEN_EL3_SVC_VAULT_ENCRYPT_64:
	case VEN_EL3_SVC_VAULT_ENCRYPT_32: {
		uint64_t in_blk[2] = { (uint64_t)x1, (uint64_t)x2 };
		uint64_t out_blk[2] = { 0 };
		vault_aes128_encrypt((const uint8_t *)in_blk, (uint8_t *)out_blk, secret_vault_key);
		SMC_RET2(handle, out_blk[0], out_blk[1]);
		break;
	}
	case VEN_EL3_SVC_VAULT_DECRYPT_64:
	case VEN_EL3_SVC_VAULT_DECRYPT_32: {
		uint64_t in_blk[2] = { (uint64_t)x1, (uint64_t)x2 };
		uint64_t out_blk[2] = { 0 };
		vault_aes128_decrypt((const uint8_t *)in_blk, (uint8_t *)out_blk, secret_vault_key);
		SMC_RET2(handle, out_blk[0], out_blk[1]);
		break;
	}
	default:
		WARN("Unimplemented vendor-specific EL3 Service call: 0x%x\n", smc_fid);
		SMC_RET1(handle, SMC_UNK);
		break;
	}
```
* **Lines 348–377**: The dispatch table:
  * `VEN_EL3_SVC_UID` (`0x8700ff01`): Returns the 128-bit UUID across registers `x0`–`x3`.
  * `VEN_EL3_SVC_VERSION` (`0x8700ff03`): Returns Major/Minor version.
  * **Encryption Case (`0xC7000040` / `0x87000040`)**:
    1. Reassembles the 16 bytes of input data from registers `x1` (lower 8 bytes) and `x2` (upper 8 bytes).
    2. Invokes `vault_aes128_encrypt` using `secret_vault_key`.
    3. Writes the 16 bytes of ciphertext into `out_blk`.
    4. Calls macro `SMC_RET2(handle, out_blk[0], out_blk[1])`, which stores `out_blk[0]` into saved register `x0` and `out_blk[1]` into saved register `x1` inside `handle`.
  * **Decryption Case (`0xC7000041` / `0x87000041`)**:
    1. Reassembles the 16 bytes of ciphertext from `x1` and `x2`.
    2. Invokes `vault_aes128_decrypt` using `secret_vault_key`.
    3. Calls `SMC_RET2` to place the decrypted plaintext into return registers `x0` and `x1`.
  * **Default Case**: Returns `SMC_UNK` (`-1` = `0xffffffffffffffff`) in register `x0` for unhandled function numbers.

```c
DECLARE_RT_SVC(
	ven_el3_svc,
	OEN_VEN_EL3_START,
	OEN_VEN_EL3_END,
	SMC_TYPE_FAST,
	ven_el3_svc_setup,
	ven_el3_svc_handler
);
```
* **Lines 380–387**: Registers the runtime service descriptor.
  * Macro `DECLARE_RT_SVC` places a `rt_svc_desc_t` structure into the linker section `.rt_svc_descs`.
  * At boot, TF-A iterates through `.rt_svc_descs` between `__RT_SVC_DESCS_START__` and `__RT_SVC_DESCS_END__`.
  * Whenever an SMC with OEN 7 (`OEN_VEN_EL3_START` = 7) and Fast type is triggered, TF-A's dispatcher branches directly to `ven_el3_svc_handler`.

---

### 1.2 Build Integration: `trusted-firmware-a/plat/qemu/qemu/platform.mk`

To link `ven_el3_svc.c` into the BL31 binary without breaking dependencies, we modified the platform Makefile:

```makefile
ifeq (${ARCH},aarch64)
BL31_SOURCES		+=	drivers/arm/pl061/pl061_gpio.c		\
				drivers/gpio/gpio.c			\
				plat/common/plat_hold_pen.c		\
				${PLAT_QEMU_COMMON_PATH}/qemu_pm.c	\
				${PLAT_QEMU_COMMON_PATH}/topology.c	\
				services/el3/ven_el3_svc.c
```
* **Lines 160–166**: Appends `services/el3/ven_el3_svc.c` to `BL31_SOURCES`.
* **Design Decision**: Upstream TF-A originally included `ven_el3_svc.c` only if `ENABLE_PMF=1` or `USE_DEBUGFS=1`. By explicitly appending `services/el3/ven_el3_svc.c` to `BL31_SOURCES` directly in the QEMU platform definition, we ensure the service is built unconditionally without bringing in the complex Performance Measurement Framework (PMF) that causes undefined reference errors on QEMU.

---

## SECTION 2: Linux Kernel Driver Layer (EL1)

### 2.1 Shared Header: `sec_vault/sec_vault_ioctl.h`

This header defines the ABI (Application Binary Interface) between the Linux Kernel module and user space.

```c
#ifndef SEC_VAULT_IOCTL_H
#define SEC_VAULT_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif
```
* **Lines 1–10**: Dual-compilation preprocessor guards:
  * When compiled inside the kernel module (`__KERNEL__` defined), it includes Linux kernel headers (`linux/types.h`, `linux/ioctl.h`).
  * When compiled in user space (for `vault_app.c`), it includes POSIX user-space headers (`stdint.h`, `sys/ioctl.h`).

```c
struct sec_vault_block {
	uint8_t data[16];
};
```
* **Lines 12–14**: Represents an atomic 16-byte (128-bit) cryptographic block.

```c
#define SEC_VAULT_MAGIC 'v'

#define SEC_VAULT_IOCTL_ENCRYPT _IOWR(SEC_VAULT_MAGIC, 1, struct sec_vault_block)
#define SEC_VAULT_IOCTL_DECRYPT _IOWR(SEC_VAULT_MAGIC, 2, struct sec_vault_block)

#endif /* SEC_VAULT_IOCTL_H */
```
* **Lines 16–21**: Defines the IOCTL commands using the standard Linux macro:
  `_IOWR(type, nr, datatype)`
  * `SEC_VAULT_MAGIC` (`'v'` = `0x76`): The unique 8-bit subsystem identifier.
  * Command numbers: `1` for Encrypt, `2` for Decrypt.
  * `_IOWR`: Encodes both **Read** and **Write** directions into the 32-bit command number because data travels both to and from the kernel (user passes plaintext, receives ciphertext; or user passes ciphertext, receives plaintext).

---

### 2.2 Kernel Module Source: `sec_vault/sec_vault.c`

This driver bridges user space requests to EL3 firmware instructions.

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/arm-smccc.h>

#include "sec_vault_ioctl.h"
```
* **Lines 1–9**:
  * `miscdevice.h`: Required for registering `/dev/sec_vault` with an automatically assigned minor number.
  * `uaccess.h`: Provides safe memory copying between User Space and Kernel Space (`copy_from_user`, `copy_to_user`).
  * `arm-smccc.h`: Official Linux header providing the ARM SMC Calling Convention interface (`arm_smccc_smc`).

```c
#define SMC_SEC_VAULT_ENCRYPT 0xC7000040UL
#define SMC_SEC_VAULT_DECRYPT 0xC7000041UL
```
* **Lines 11–12**: Matches the 64-bit SMC Function IDs defined in the firmware.

```c
static long sec_vault_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sec_vault_block blk;
	struct arm_smccc_res res;
	uint64_t *p64 = (uint64_t *)blk.data;

	if (copy_from_user(&blk, (void __user *)arg, sizeof(blk)))
		return -EFAULT;
```
* **Lines 14–22**:
  * `copy_from_user(&blk, (void __user *)arg, sizeof(blk))`: Securely copies 16 bytes from user-space virtual memory (governed by `TTBR0_EL1`) into the kernel's local stack frame (governed by `TTBR1_EL1`). If the user passes an invalid or unmapped pointer, the function returns `-EFAULT` (`Bad Address`), preventing kernel crashes.
  * `uint64_t *p64 = (uint64_t *)blk.data`: Casts the byte array to two 64-bit unsigned integers to facilitate loading directly into ARM64 64-bit general-purpose CPU registers `x1` and `x2`.

```c
	switch (cmd) {
	case SEC_VAULT_IOCTL_ENCRYPT:
		/* Pass 16-byte block across registers X1 and X2 to EL3 */
		arm_smccc_smc(SMC_SEC_VAULT_ENCRYPT,
			      p64[0], p64[1], 0, 0, 0, 0, 0,
			      &res);
		p64[0] = res.a0;
		p64[1] = res.a1;
		break;
```
* **Lines 25–33**:
  * Calls `arm_smccc_smc()`: This inline assembly function loads `x0 = 0xC7000040`, `x1 = p64[0]`, `x2 = p64[1]`, and executes `smc #0`.
  * The processor immediately freezes Linux execution at EL1 and switches into EL3.
  * When EL3 returns via `eret`, `res.a0` holds the first 8 bytes of ciphertext, and `res.a1` holds the last 8 bytes.
  * Overwrites `p64[0]` and `p64[1]` with the returned ciphertext.

```c
	case SEC_VAULT_IOCTL_DECRYPT:
		/* Pass 16-byte block across registers X1 and X2 to EL3 */
		arm_smccc_smc(SMC_SEC_VAULT_DECRYPT,
			      p64[0], p64[1], 0, 0, 0, 0, 0,
			      &res);
		p64[0] = res.a0;
		p64[1] = res.a1;
		break;

	default:
		return -EINVAL;
	}
```
* **Lines 35–45**: Decryption branch. If the caller passes an unsupported command, returns `-EINVAL` (`Invalid Argument`).

```c
	if (copy_to_user((void __user *)arg, &blk, sizeof(blk)))
		return -EFAULT;

	return 0;
}
```
* **Lines 47–51**: `copy_to_user()` writes the resulting 16 bytes back into user-space memory, and returns `0` (Success).

```c
static const struct file_operations sec_vault_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = sec_vault_ioctl,
	.compat_ioctl   = sec_vault_ioctl,
};

static struct miscdevice sec_vault_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "sec_vault",
	.fops  = &sec_vault_fops,
	.mode  = 0666, /* World-accessible for testing */
};
```
* **Lines 53–64**:
  * `sec_vault_fops`: Binds `unlocked_ioctl` and `compat_ioctl` (for 32-bit compatibility if needed) to `sec_vault_ioctl`.
  * `sec_vault_device`: Configures a miscellaneous character driver:
    * `MISC_DYNAMIC_MINOR`: Instructs the Linux kernel to automatically assign an unused minor number under major 10.
    * `name = "sec_vault"`: Creates the device file node automatically at `/dev/sec_vault` via `devtmpfs`.
    * `mode = 0666`: Grants read and write permissions to all user-space processes.

```c
static int __init sec_vault_init(void)
{
	int ret;

	ret = misc_register(&sec_vault_device);
	if (ret) {
		pr_err("sec_vault: failed to register /dev/sec_vault: %d\n", ret);
		return ret;
	}

	pr_info("sec_vault: Registered /dev/sec_vault (Hardware Security Vault)\n");
	return 0;
}

static void __exit sec_vault_exit(void)
{
	misc_deregister(&sec_vault_device);
	pr_info("sec_vault: Unloaded /dev/sec_vault\n");
}

module_init(sec_vault_init);
module_exit(sec_vault_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity TrustZone Project");
MODULE_DESCRIPTION("Hardware Security Module (HSM) Vault Driver");
```
* **Lines 66–91**: Standard Linux driver lifecycle functions registering on `insmod` and deregistering on `rmmod`.

---

### 2.3 Kernel Makefile: `sec_vault/Makefile`

```makefile
obj-m += sec_vault.o

KDIR ?= ../linux
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
```
* **Line 1**: `obj-m += sec_vault.o` instructs kbuild to build `sec_vault.c` into a loadable kernel module (`sec_vault.ko`).
* **Line 3**: `KDIR ?= ../linux` points to the kernel source directory where `vmlinux` and the `Module.symvers` symbols reside.
* **Line 7–9**: Executes the kernel's top-level Makefile (`-C $(KDIR)`), passing the current directory (`M=$(CURDIR)`) to build out-of-tree modules targeting `arm64` using cross-compiler `aarch64-linux-gnu-`.

---

## SECTION 3: User Space Layer (EL0)

### 3.1 Application Source: `vault_app.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "sec_vault/sec_vault_ioctl.h"
```
* **Lines 1–7**: Includes standard POSIX I/O and the shared ABI header `sec_vault/sec_vault_ioctl.h`.

```c
static void print_hex(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		printf("%02x ", data[i]);
	}
	printf("\n");
}
```
* **Lines 15–21**: Helper function printing 16-byte binary buffers in two-character hexadecimal format.

```c
int main(void)
{
	int fd;
	struct sec_vault_block block;
	uint8_t saved_ciphertext[16];

	fd = open("/dev/sec_vault", O_RDWR);
	if (fd < 0) {
		perror("[-] Error: Failed to open /dev/sec_vault");
		return 1;
	}
```
* **Lines 23–39**: Opens `/dev/sec_vault`. The Linux kernel maps this file descriptor to the `file_operations` table registered by `sec_vault.ko`.

```c
	/* 1. Original Plaintext */
	memset(block.data, 0, sizeof(block.data));
	memcpy(block.data, "SecretTrustZone!", 16);

	printf("[1] Original Plaintext (EL0) : \"%.16s\"\n", (char *)block.data);
	printf("    Plaintext Hex            : ");
	print_hex(block.data, 16);
```
* **Lines 41–48**: Prepares the 16-byte ASCII test payload: `"SecretTrustZone!"`.

```c
	/* 2. Request EL3 to encrypt using the hidden key */
	printf("\n[*] Invoking ioctl(SEC_VAULT_IOCTL_ENCRYPT)...\n");

	if (ioctl(fd, SEC_VAULT_IOCTL_ENCRYPT, &block) < 0) {
		perror("[-] ioctl encrypt failed");
		close(fd);
		return 1;
	}

	printf("[2] Ciphertext received from EL3: ");
	print_hex(block.data, 16);
	memcpy(saved_ciphertext, block.data, 16);
```
* **Lines 50–63**:
  * Calls `ioctl(fd, SEC_VAULT_IOCTL_ENCRYPT, &block)`.
  * The CPU executes `svc #0` $\rightarrow$ drops into `sec_vault.ko` at EL1 $\rightarrow$ executes `smc #0` $\rightarrow$ drops into TF-A at EL3 $\rightarrow$ executes AES-128 $\rightarrow$ returns ciphertext back to `block.data`.
  * The received ciphertext is:
    `2d 6c 2c e1 d3 eb cd c5 cb 4d bf 1d 4f f3 c6 07`.

```c
	/* 3. Corrupt memory to demonstrate memory zeroing */
	memset(block.data, 0x00, 16);
	printf("    Memory wiped in user space  : ");
	print_hex(block.data, 16);
```
* **Lines 65–68**: Crucial security step: `memset` overwrites `block.data` in user-space RAM with all zeros (`0x00`). This demonstrates that user space is no longer storing the plaintext.

```c
	/* 4. Request EL3 to decrypt using the hidden key */
	memcpy(block.data, saved_ciphertext, 16);

	if (ioctl(fd, SEC_VAULT_IOCTL_DECRYPT, &block) < 0) {
		perror("[-] ioctl decrypt failed");
		close(fd);
		return 1;
	}

	printf("[3] Decrypted Plaintext from EL3: \"%.16s\"\n", (char *)block.data);
	printf("    Decrypted Hex               : ");
	print_hex(block.data, 16);
```
* **Lines 70–84**: Hands the ciphertext back to EL3 via `SEC_VAULT_IOCTL_DECRYPT`. EL3 decrypts the block using the same hidden key, returning the original `"SecretTrustZone!"`.

```c
	/* 5. Verification */
	if (memcmp(block.data, "SecretTrustZone!", 16) == 0) {
		printf("\n[+] SUCCESS: Decrypted message matches original perfectly!\n");
	} else {
		printf("\n[-] ERROR: Decrypted message mismatch!\n");
	}
```
* **Lines 86–91**: Confirms that `memcmp` returns 0 (exact byte-for-byte match).

---

## SECTION 4: Root Filesystem & System Initialization

### 4.1 System Init Script: `initramfs_work/init`

The Linux kernel launches this script as Process ID 1 (`PID 1`) inside the in-memory root filesystem.

```sh
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

# Automatically load hardware security drivers
if [ -f /sec_vault.ko ]; then
    insmod /sec_vault.ko
fi
if [ -f /el3_uuid.ko ]; then
    insmod /el3_uuid.ko
fi

echo ""
echo "============================================="
echo "  Welcome to Minimal ARM64 Embedded Linux!  "
echo "============================================="
echo ""

exec /bin/sh
```
* **Lines 2–4**: Mounts the three essential virtual filesystems:
  * `/proc`: Kernel process information and system status.
  * `/sys`: Sysfs device hierarchy.
  * `/dev` (`devtmpfs`): Crucial for device nodes. When `sec_vault.ko` registers itself via `misc_register()`, `devtmpfs` automatically synthesizes the character node `/dev/sec_vault` with permissions `0666`.
* **Lines 6–12**: Automatically inserts the kernel modules using `insmod` without requiring manual user intervention at boot.
* **Line 19**: `exec /bin/sh` replaces the `init` script process image with the BusyBox interactive command shell, presenting the prompt `~ #`.

---

## SECTION 5: Storage Disk & Firmware Image Layouts

### 5.1 The Firmware ROM: `arm64_scratch/flash.bin` (64 MB)

This image is loaded by QEMU's `-bios flash.bin` option starting at physical address `0x00000000`:

```
Offset 0x00000000 ┌──────────────────────────────────────────────────────────┐
                  │ BL1: Boot ROM (bl1.bin - 22,960 Bytes)                   │
                  │ • Reset Vector for all 4 cores                           │
                  │ • Parks secondary cores 1-3 via WFE                      │
                  │ • Reads FIP at 0x40000                                   │
Offset 0x000059B0 ├──────────────────────────────────────────────────────────┤
                  │ Zero Padding (233 KB)                                    │
Offset 0x00040000 ├──────────────────────────────────────────────────────────┤
                  │ FIP: Firmware Image Package (fip.bin - 1.6 MB)           │
                  │   ├── BL2: Trusted Boot Firmware (Secure EL1)            │
                  │   ├── BL31: EL3 Runtime Monitor (Contains our AES Vault) │
                  │   └── BL33: Das U-Boot Bootloader (Non-Secure EL2)       │
Offset 0x001C7D3F ├──────────────────────────────────────────────────────────┤
                  │ Zero Padding (62.2 MB)                                   │
Offset 0x04000000 └──────────────────────────────────────────────────────────┘ (64 MB Total)
```

### 5.2 The Virtual Hard Disk: `arm64_scratch/boot_disk_test.img` (128 MB)

Attached via `-drive if=none,file=boot_disk_test.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0`.
Formatted with **FAT16**:

* **Sector Size**: 512 Bytes.
* **Cluster Size**: 4 Sectors = 2048 Bytes (2 KB).
* **Reserved Sectors**: 4 (Boot sector and reserved area).
* **FAT Tables**: 2 tables $\times$ 256 sectors each.
* **Root Directory**: Starts at byte offset `0x40800` (Sector 516), holding 512 entries.
* **Data Area**: Starts at byte offset `0x44800` (Sector 548).

**Allocated Files in the FAT16 Filesystem:**
1. **`IMAGE`** (`Image`):
   * Size: 52,369,920 Bytes (50 MB).
   * Starting Cluster: Cluster 3.
   * Extent: Clusters 3 through 25,574 (linear contiguous allocation).
2. **`INITRA~1.GZ`** (`initramfs.cpio.gz`):
   * Size: 2,101,399 Bytes (2.0 MB).
   * Starting Cluster: Cluster 25,575.
   * Extent: Clusters 25,575 through 26,601 (updated via our direct FAT16 cluster allocator).

---

## SECTION 6: Complete Orchestration & Boot Commands

### 6.1 QEMU Launch Command (`commands.txt`)

```bash
qemu-system-aarch64 \
  -M virt,secure=on,virtualization=on \
  -cpu cortex-a53 \
  -smp 4 \
  -m 1024M \
  -nographic \
  -bios flash.bin \
  -drive if=none,file=boot_disk_test.img,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0
```

| Flag | Exact Purpose & Hardware Architectural Meaning |
| :--- | :--- |
| `-M virt` | Instantiates QEMU's ARM Virtual Platform board (`hw/arm/virt.c`). |
| `secure=on` | Enables **ARM TrustZone**. Instantiates Secure Memory, EL3, and Flash 0 (`0x00000000`). |
| `virtualization=on` | Enables **EL2 (Hypervisor mode)**, required for U-Boot and KVM. |
| `-cpu cortex-a53` | Emulates the 64-bit ARM Cortex-A53 microarchitecture (ARMv8.0-A, 8-stage pipeline). |
| `-smp 4` | Configures 4 symmetric CPU cores (Cores 0, 1, 2, 3). |
| `-m 1024M` | Allocates 1024 MB (1 GB) of physical DRAM starting at physical address `0x40000000`. |
| `-nographic` | Disables emulated graphical display and redirects PL011 UART to the host terminal standard I/O. |
| `-bios flash.bin` | Maps `flash.bin` as the boot ROM into Flash 0 at `0x00000000`. |
| `-drive if=none...` | Registers `boot_disk_test.img` as an unattached raw storage backend named `hd0`. |
| `-device virtio-blk...` | Attaches `hd0` to the CPU bus as a high-speed **VirtIO Memory-Mapped I/O (MMIO)** block controller. |

---

### 6.2 U-Boot Boot Commands (`commands.txt`)

Executed interactively at the U-Boot prompt `=>`:

```text
# 1. Load the Linux kernel Image into RAM at physical address 0x40400000
fatload virtio 0 0x40400000 Image

# 2. Load the initial RAM disk into RAM at physical address 0x44000000
fatload virtio 0 0x44000000 initramfs.cpio.gz

# 3. Configure the kernel serial console and root init process
setenv bootargs "console=ttyAMA0 earlycon rdinit=/init"

# 4. Boot the ARM64 kernel
booti 0x40400000 0x44000000:$filesize $fdtcontroladdr
```

* **`fatload virtio 0 0x40400000 Image`**: U-Boot's VirtIO driver queries block controller `virtio 0`, traverses the FAT16 directory table, and copies the 52 MB kernel into physical RAM starting at `0x40400000`.
* **`fatload virtio 0 0x44000000 initramfs.cpio.gz`**: Copies the 2.1 MB rootfs archive into RAM at `0x44000000`. Sets U-Boot environment variable `$filesize` to the number of bytes read (`0x201097`).
* **`setenv bootargs ...`**: Passes boot arguments:
  * `console=ttyAMA0`: Primary console on the ARM PL011 UART (`0x09000000`).
  * `earlycon`: Enables early UART output before the full tty driver initializes.
  * `rdinit=/init`: Tells the kernel to execute `/init` from the RAM disk instead of mounting an external root filesystem.
* **`booti 0x40400000 0x44000000:$filesize $fdtcontroladdr`**:
  * Satisfies the ARM64 boot protocol: cleans and disables caches, turns MMU **OFF**, sets CPU register `x0` to the Flattened Device Tree address (`$fdtcontroladdr`), and branches to `0x40400000`.

---

## SECTION 7: Comprehensive Design Decisions & Trade-Offs

### 1. Register-Based SMC Passing vs. Shared Memory Pages
* **Chosen Approach**: Passing the 16-byte block across CPU registers `x1` and `x2`.
* **Rationale**: Passing data via registers is completely atomic, operates in constant time, leaves no residual data in RAM, and requires zero shared memory page table remapping between Secure and Non-Secure worlds.

### 2. Standalone AES-128 Implementation vs. External Crypto Libraries
* **Chosen Approach**: Built an in-tree, zero-dependency AES-128 block implementation directly into `ven_el3_svc.c`.
* **Rationale**: Eliminates external dependencies on mbedTLS or OpenSSL, ensuring TF-A compiles in under 4 seconds without missing symbol link errors.

### 3. Miscellaneous Device (`miscdevice`) vs. Standard Character Driver
* **Chosen Approach**: Used Linux `miscdevice` API (`MISC_DYNAMIC_MINOR`).
* **Rationale**: Avoids hardcoding or requesting major character numbers via `alloc_chrdev_region()`, automatically creating the `/dev/sec_vault` device node via `devtmpfs`.

### 4. Static User-Space Linking
* **Chosen Approach**: Cross-compiled `vault_app.c` with `-static`.
* **Rationale**: Guarantees the binary runs seamlessly on minimal BusyBox root filesystems without requiring shared C libraries (`libc.so`) or dynamic linkers (`ld-linux-aarch64.so.1`).
