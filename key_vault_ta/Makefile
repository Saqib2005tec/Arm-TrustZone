CFG_TEE_TA_LOG_LEVEL ?= 4
CPPFLAGS += -DCFG_TEE_TA_LOG_LEVEL=$(CFG_TEE_TA_LOG_LEVEL)

# The UUID of the Key Vault Trusted Application
BINARY = e5b22b10-6c34-4b55-a0ef-28e4695e7b23

TA_DEV_KIT_DIR ?= /home/gg-gamer/embedde_gg/linux_gg/arm64_scratch/optee_os/out/arm-plat-vexpress/export-ta_arm64

include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk

