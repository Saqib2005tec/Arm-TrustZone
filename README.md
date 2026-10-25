# ARMv8-A TrustZone & OP-TEE Hardware Key Vault

[![Architecture](https://img.shields.io/badge/Architecture-ARMv8--A%20(AArch64)-blue.svg)](#)
[![SoC](https://img.shields.io/badge/Target%20CPU-Cortex--A53-orange.svg)](#)
[![Firmware](https://img.shields.io/badge/Secure%20Monitor-TF--A%20v2.15-red.svg)](#)
[![TEE](https://img.shields.io/badge/Trusted%20OS-OP--TEE%20v4.5.0-green.svg)](#)
[![Host OS](https://img.shields.io/badge/Kernel-Linux%20v7.3-yellow.svg)](#)
[![API Standard](https://img.shields.io/badge/Standard-GlobalPlatform%20TEE-purple.svg)](#)

A complete, production-grade **Hardware Security Module (HSM) Key Vault** implemented on **ARMv8-A TrustZone** from the ground up. 

This project demonstrates how to execute cryptographic operations (encryption, decryption, verification) from an unprivileged Linux user-space application using a high-value master cryptographic key **without the host Linux operating system ever being able to read, access, or compromise the key itself**.

> 📄 **Exhaustive Technical Documentation:** A full 11-page publication-grade PDF specification and architecture manual is available in [`Documentation/ARM_TrustZone_OPTEE_Key_Vault_Specification.pdf`](Documentation/ARM_TrustZone_OPTEE_Key_Vault_Specification.pdf).

---

## 🏛️ System Architecture Topology

The system enforces spatial hardware isolation using ARM TrustZone, partitioning the Cortex-A53 processor and physical RAM into two worlds:

```
                           ARMv8-A TRUSTZONE ARCHITECTURE
       NORMAL WORLD (NS=1)                │               SECURE WORLD (NS=0)
                                          │
 [EL0] User Application: vault_client     │  [S-EL0] Trusted Application: key_vault_ta
       (Invokes libteec APIs)             │          ★ SECRET 128-BIT KEY IS HERE ★
               │                          │                       ▲
               ▼                          │                       │ (svc #0 System Call)
 [EL1] Linux Kernel (v7.3)                │  [S-EL1] OP-TEE OS (BL32 Microkernel)
       (drivers/tee/optee.ko /dev/tee0)   │          (MMU, Pager, Scheduler, Crypto)
               │                          │                       ▲
               ▼ (smc #0 Call)            │                       │ (eret Jump)
 ─────────────────────────────────────────┴───────────────────────┴─────────────────────────
 [EL3] ARM Trusted Firmware (TF-A BL31 + SPD: opteed)
       (Secure Monitor: Saves/Restores Context, Flips SCR_EL3.NS bit)
```

### Four-Tier Security Matrix

| Exception Level | Normal World (NS=1) | Secure World (NS=0) | Security Role |
| :--- | :--- | :--- | :--- |
| **EL0 (User)** | `vault_client`, BusyBox shell | `key_vault_ta` | Isolated execution of untrusted vs. trusted applications |
| **EL1 (Kernel / OS)** | Linux Kernel v7.3 (`/dev/tee0`) | OP-TEE OS (BL32 microkernel) | Rich OS vs. Trusted OS microkernel |
| **EL2 (Hypervisor)** | KVM / Hypervisor | Secure EL2 (SPMC) | Virtualization boundaries |
| **EL3 (Monitor)** | *N/A (EL3 is always Secure)* | ARM Trusted Firmware (TF-A) | Secure Monitor & Exception Dispatcher (`opteed`) |

---

## 🧠 Memory Map & Hardware Firewalling

### Physical Memory Allocation (QEMU `virt`)

```
 0x00000000
      │
      ▼
 ┌────────────────────────────────────────────────────────┐
 │ 0x0E100000 - 0x0EFFFFFF (15 MB)                        │
 │ SECURE DRAM (TZDRAM)                                   │◄─── HARDWARE FIREWALL (TZASC-400)
 │ • OP-TEE OS Kernel (S-EL1)                             │     Normal World access (NS=1)
 │ • Key Vault TA & 128-bit Key (S-EL0)                   │     triggers External Data Abort!
 ├────────────────────────────────────────────────────────┤
 │ 0x40000000 - 0x403FFFFF (4 MB)                         │
 │ Reserved low RAM & spin-tables                         │
 ├────────────────────────────────────────────────────────┤
 │ 0x40400000 - 0x437FFFFF (~52 MB)                       │
 │ Linux Kernel Image (Executable code & static data)     │
 ├────────────────────────────────────────────────────────┤
 │ 0x44000000 - 0x443FFFFF (~4 MB)                        │
 │ Initramfs Root Filesystem (BusyBox in RAM)             │
 ├────────────────────────────────────────────────────────┤
 │ 0x45000000 - 0x451FFFFF (2 MB)                         │
 │ SHARED MEMORY (SHM)                                    │◄─── ZERO-COPY CROSS-WORLD MAILBOX
 │ • Linux and OP-TEE BOTH have access                    │     Used for parameters & data payloads
 ├────────────────────────────────────────────────────────┤
 │ 0x45200000 - 0x7FFFFFFF (~942 MB)                      │
 │ Linux Dynamic Memory Pool (Buddy allocator, heap, stack)
 └────────────────────────────────────────────────────────┘
```

### The TrustZone Address Space Controller (TZASC-400)
* **Hardware Isolation:** Situated on the internal system interconnect (AXI bus), the TZASC filters memory accesses by inspecting the electrical **NS bit**.
* **Enforcement:** If code executing in Linux (NS=1) attempts to load or store to `0x0E100000`, the memory controller aborts the transaction immediately.

### Custom Shared Memory (`CFG_SHMEM_START=0x45000000`)
Upstream OP-TEE defaults `CFG_SHMEM_START` to `0x42000000`. Because our Linux kernel image is ~52 MB (spanning `0x40400000` to `0x43800000`), the default shared memory would corrupt the running kernel. We explicitly configured `CFG_SHMEM_START=0x45000000` (2 MB) to sit cleanly above initramfs.

---

## 📁 Repository Structure

```text
.
├── Documentation/
│   ├── ARM_TrustZone_OPTEE_Key_Vault_Specification.pdf  # Comprehensive 11-page technical manual
│   ├── ARM_TrustZone_OPTEE_Key_Vault_Specification.md   # Markdown technical manual
│   └── generate_pdf_report.py                           # Python generator for specification PDF
├── key_vault_ta/                                        # Secure EL0 Trusted Application
│   ├── key_vault_ta.h                                   # Public header (UUID, Command IDs)
│   ├── user_ta_header_defines.h                         # TA runtime properties & stack/heap limits
│   ├── key_vault_ta.c                                   # TA implementation & secret key
│   ├── sub.mk                                           # Build specification
│   ├── Makefile                                         # Links against OP-TEE TA DevKit
│   └── e5b22b10-6c34-4b55-a0ef-28e4695e7b23.ta         # Signed Trusted Application binary
├── vault_client/                                        # Normal World EL0 Linux Application
│   └── vault_client.c                                   # Client app utilizing GlobalPlatform libteec
├── boot.cmd                                             # U-Boot boot script source
├── boot.scr                                             # Compiled U-Boot image script (mkimage)
├── extlinux.conf                                        # Standard Extlinux configuration
├── commands.txt                                         # Command-line cheat sheet
├── optee.log                                            # Verified dual-UART trace from OP-TEE OS
└── README.md                                            # Project overview & reproduction guide
```

---

## 🔐 Key Vault Trusted Application (`key_vault_ta`)

The Trusted Application conforms to the **GlobalPlatform TEE Internal Core API v1.1**:

* **UUID:** `e5b22b10-6c34-4b55-a0ef-28e4695e7b23`
* **Secret Key:** 128-bit key (`"KEY_VAULT_SECRET"`) stored in the `.rodata` section in Secure EL0 DRAM.
* **Early TA Deployment:** Compiled into an Early TA and embedded directly into `tee-pager_v2.bin` via `EARLY_TA_PATHS`. It requires no background supplicant or dynamic disk loading.

### Command Interfaces:
1. `TA_KEY_VAULT_CMD_GET_INFO (0)`: Probes vault health. Returns `key_size = 128` bits and `status = 0xCAFE`.
2. `TA_KEY_VAULT_CMD_ENCRYPT (1)`: Encrypts an input memory reference buffer using the secret key and keystream cipher, returning ciphertext in the output buffer.
3. `TA_KEY_VAULT_CMD_DECRYPT (2)`: Reverses the cipher transformation to verify cryptographic symmetry.

---

## 💻 Linux Client Application (`vault_client`)

Running in Normal World user space (EL0), `vault_client.c` uses the official **GlobalPlatform TEE Client API** (`libteec`):

1. `TEEC_InitializeContext()`: Opens `/dev/tee0` to communicate with the kernel TEE driver.
2. `TEEC_OpenSession()`: Traps across TrustZone to instantiate a session with TA `e5b22b10-6c34...`.
3. `TEEC_InvokeCommand(GET_INFO)`: Queries vault status.
4. `TEEC_InvokeCommand(ENCRYPT)`: Transmits plaintext across the border via Shared Memory, receiving ciphertext.
5. `TEEC_InvokeCommand(DECRYPT)`: Transmits ciphertext back across the border, receiving original plaintext.
6. `TEEC_CloseSession()` & `TEEC_FinalizeContext()`: Gracefully releases resources.

---

## 🚀 How to Build & Run from Scratch

### 1. Prerequisites
Install the ARM64 cross-compiler toolchain and QEMU:
```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
                 qemu-system-arm device-tree-compiler python3-pyelftools
```

### 2. Compile Key Vault TA
```bash
make -C key_vault_ta \
     CROSS_COMPILE=aarch64-linux-gnu- \
     TA_DEV_KIT_DIR=/path/to/optee_os/out/arm-plat-vexpress/export-ta_arm64
```

### 3. Compile OP-TEE OS with Embedded Early TA
```bash
make -C optee_os \
     PLATFORM=vexpress-qemu_armv8a \
     CFG_ARM64_core=y \
     CROSS_COMPILE64=aarch64-linux-gnu- \
     CFG_SHMEM_START=0x45000000 \
     EARLY_TA_PATHS=$(pwd)/key_vault_ta/e5b22b10-6c34-4b55-a0ef-28e4695e7b23.stripped.elf \
     -j$(nproc)
```

### 4. Package ARM Trusted Firmware (TF-A)
```bash
make -C trusted-firmware-a \
     CROSS_COMPILE=aarch64-linux-gnu- \
     PLAT=qemu \
     BL33=/path/to/u-boot/u-boot.bin \
     SPD=opteed \
     BL32=/path/to/optee_os/out/arm-plat-vexpress/core/tee-header_v2.bin \
     BL32_EXTRA1=/path/to/optee_os/out/arm-plat-vexpress/core/tee-pager_v2.bin \
     BL32_EXTRA2=/path/to/optee_os/out/arm-plat-vexpress/core/tee-pageable_v2.bin \
     BL32_RAM_LOCATION=tdram \
     fip qemu_fw.rom

cp trusted-firmware-a/build/qemu/release/qemu_fw.rom flash.bin
```

### 5. Compile Linux Client
```bash
aarch64-linux-gnu-gcc -static \
    -I/path/to/optee_client/libteec/include \
    -I$(pwd)/key_vault_ta \
    vault_client/vault_client.c \
    /path/to/optee_client/out/libteec/libteec.a \
    -lpthread \
    -o vault_client/vault_client
```

### 6. Create MBR Partitioned Hard Drive (`boot_disk.img`)
Format an industry-standard MBR partition table with Sector 2048 alignment:
```bash
dd if=/dev/zero of=boot_disk.img bs=1M count=128
parted -s boot_disk.img mklabel msdos mkpart primary fat16 1MiB 100% set 1 boot on
mkfs.vfat -F 16 --offset=2048 -n "BOOT" boot_disk.img
```
Copy `Image`, `initramfs.cpio.gz`, and `/extlinux/extlinux.conf` into Partition 1.

---

## ⚡ Running in QEMU & Verification

Launch the system with dual-UART serial support:

```bash
qemu-system-aarch64 \
  -M virt,secure=on,virtualization=on \
  -cpu cortex-a53 \
  -smp 4 \
  -m 1024M \
  -nographic \
  -bios flash.bin \
  -drive if=none,file=boot_disk.img,format=raw,id=hd0,file.locking=off \
  -device virtio-blk-device,drive=hd0 \
  -serial mon:stdio \
  -serial file:optee.log
```

### Normal World User Shell (UART 0)
Once booted to the BusyBox shell (`~ #`), run:

```sh
~ # vault_client
=======================================================
   ARM TrustZone Key Vault Client (Normal World EL0)  
=======================================================
[+] Successfully connected to /dev/tee0
[+] Secure Session OPENED with Key Vault TA in Secure EL0!
[+] Key Vault Info: KeySize=128 bits, Status=0xCAFE

[Original Plaintext] : "TOP_SECRET: My password is SuperSecretPassword123!" (len=50)
[+] Encrypted via S-EL0 Key Vault! Ciphertext bytes:
    2A 28 07 24 64 68 76 65 3A 3A 38 6C 44 4A 4B 43 
    56 50 6B 5C 1C 10 0E 1D 42 42 40 14 06 02 01 0B 
    02 0C 05 0F 70 7A 77 75 2E 28 26 2F 58 54 53 59 
    50 5E 
[+] Decrypted via S-EL0 Key Vault: "TOP_SECRET: My password is SuperSecretPassword123!"

[SUCCESS] Cryptographic loop verified! Secret key was used without Linux ever reading it!
=======================================================
```

### Secure World Log (UART 1: `optee.log`)
```text
I/TC: Primary CPU switching to normal world boot
I/TC: Reserved shared memory is enabled (0x45000000, 2MB)
I/TA: ====================================================
I/TA: [KeyVault-TA] Created in Secure EL0!
I/TA: [KeyVault-TA] 128-bit Secret Key loaded into Secure RAM.
I/TA: ====================================================
I/TA: [KeyVault-TA] Normal World (Linux) opened a secure session.
I/TA: [KeyVault-TA] CMD_GET_INFO: KeySize=128 bits, Status=0xCAFE
I/TA: [KeyVault-TA] CMD_ENCRYPT: Processed 50 bytes with secret key.
I/TA: [KeyVault-TA] CMD_DECRYPT: Processed 50 bytes with secret key.
I/TA: [KeyVault-TA] Session closed.
I/TA: [KeyVault-TA] Destroyed.
```

---

## 📜 License & Acknowledgments

* **Author:** Saqib Ahmed ([Saqib2005tec](https://github.com/Saqib2005tec))
* **License:** BSD-2-Clause / GPL-2.0
* Built for educational research into ARM64 low-level computer architecture, embedded firmware design, and hardware-enforced trusted execution environments.
