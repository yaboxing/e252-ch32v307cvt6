# -----------------------------------------------------------------------------
# E252 + CH32V307VCT6 EtherCAT slave firmware
#
# Command line counterpart of the MounRiver Studio project in this directory.
# Source set, macros, include paths, linker script, startup file, optimisation
# and output directory are identical to .cproject, so both routes produce the
# same .hex/.bin image.
#
# Requirements: GNU make and the MounRiver Studio 3 toolchain
# (RISC-V Embedded GCC15, prefix riscv32-wch-elf-).
#
# Common usage:
#   make                     build elf/hex/bin in out/
#   make flash               build and program through WCH-LinkE / OpenOCD
#   make clean               remove the whole out/ directory
#   make MRS_ROOT=D:/path/to/MounRiver_Studio3    use another installation
# -----------------------------------------------------------------------------

TARGET   := e252-ch32v307cvt6
OUT      := out
OBJ      := $(OUT)/obj
LDSCRIPT := sdk/Ld/Link.ld
STARTUP  := sdk/Startup/startup_ch32v30x_D8C.S

ELF := $(OUT)/$(TARGET).elf
HEX := $(OUT)/$(TARGET).hex
BIN := $(OUT)/$(TARGET).bin
MAP := $(OUT)/$(TARGET).map

# Toolchain prefix. Use the GCC15 toolchain shipped with MounRiver Studio 3.
CROSS ?= riscv32-wch-elf-

# Toolchain and OpenOCD locations. On Windows they default to the MounRiver
# Studio 3 installation; elsewhere the toolchain is taken from PATH and OpenOCD
# from its own script search path. All four can be overridden on the command
# line or in the environment.
MRS_ROOT ?= C:/MounRiver/MounRiver_Studio3
MRS_WIN  := $(MRS_ROOT)/resources/app/resources/win32/components/WCH

ifeq ($(OS),Windows_NT)
  TOOLCHAIN_BIN ?= $(MRS_WIN)/Toolchain/RISC-V Embedded GCC15/bin
  OPENOCD       ?= $(MRS_WIN)/OpenOCD/OpenOCD/bin/openocd.exe
  OPENOCD_CFG   ?= $(MRS_WIN)/OpenOCD/OpenOCD/bin/wch-riscv.cfg
  # The MounRiver toolchain directory contains a space, so command names are
  # quoted where they are expanded in recipes.
  CC      := "$(TOOLCHAIN_BIN)/$(CROSS)gcc"
  OBJCOPY := "$(TOOLCHAIN_BIN)/$(CROSS)objcopy"
  SIZE    := "$(TOOLCHAIN_BIN)/$(CROSS)size"
else
  OPENOCD     ?= openocd
  OPENOCD_CFG ?= wch-riscv.cfg
  ifneq ($(strip $(TOOLCHAIN_BIN)),)
    CC      := "$(TOOLCHAIN_BIN)/$(CROSS)gcc"
    OBJCOPY := "$(TOOLCHAIN_BIN)/$(CROSS)objcopy"
    SIZE    := "$(TOOLCHAIN_BIN)/$(CROSS)size"
  else
    CC      := $(CROSS)gcc
    OBJCOPY := $(CROSS)objcopy
    SIZE    := $(CROSS)size
  endif
endif

# Architecture / ABI (must match the MRS project)
ARCH := -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8

# Optimisation and code generation. -g/-gdwarf-4 only adds debug sections to
# the ELF; the .hex/.bin image is unchanged.
OPT   := -Os
DEBUG ?= -g -gdwarf-4
# Empty by default: MRS and the toolchain default language standard are used
# (gnu17 for GCC 15).
CSTD  ?=

CFLAGS := $(ARCH) $(OPT) $(DEBUG) $(CSTD) -ffunction-sections -fdata-sections \
          -DUSE_STDPERIPH_DRIVER

INCLUDES := -I. \
            -IEthercat/Inc \
            -IEthercat/port \
            -Iapp \
            -Ihal \
            -Ibsp \
            -Isdk/Core \
            -Isdk/Peripheral/inc

LDFLAGS := $(ARCH) $(OPT) $(DEBUG) $(CSTD) -ffunction-sections -fdata-sections \
           -DUSE_STDPERIPH_DRIVER \
           -T $(LDSCRIPT) -nostartfiles -specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(MAP)

# Source set and link order: the order matters for RISC-V linker relaxation,
# so keep the per-directory sort identical to the MRS project.
C_SRCS := $(sort $(wildcard app/*.c)) \
          $(sort $(wildcard bsp/*.c)) \
          Ethercat/port/el9800hw.c \
          $(sort $(wildcard Ethercat/src/*.c)) \
          $(sort $(wildcard hal/*.c)) \
          $(sort $(wildcard sdk/Core/*.c)) \
          $(sort $(wildcard sdk/Peripheral/src/*.c))

# Objects use a flattened naming scheme so that every object lands in one
# directory (sdk/Core/core_riscv.c -> out/obj/sdk_Core_core_riscv.o).
define OBJNAME
$(OBJ)/$(subst /,_,$(basename $(1))).o
endef

OBJS := $(foreach s,$(C_SRCS) $(STARTUP),$(call OBJNAME,$(s)))
DEPS := $(OBJS:.o=.d)

.PHONY: all size flash clean help

all: $(HEX) $(BIN)
	@echo SIZE
	@$(SIZE) $(ELF)

size: $(ELF)
	@$(SIZE) $(ELF)

$(ELF): $(OBJS) $(LDSCRIPT) | $(OBJ)
	@echo LINK $@
	@$(CC) $(LDFLAGS) -o "$@" $(OBJS)

$(HEX): $(ELF)
	@$(OBJCOPY) -O ihex "$<" "$@"
	@echo HEX $@

$(BIN): $(ELF)
	@$(OBJCOPY) -O binary "$<" "$@"
	@echo BIN $@

define RULE_C
$(call OBJNAME,$(1)): $(1) | $(OBJ)
	@echo CC $(1)
	@$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c "$(1)" -o "$(call OBJNAME,$(1))"
endef

define RULE_S
$(call OBJNAME,$(1)): $(1) | $(OBJ)
	@echo AS $(1)
	@$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c "$(1)" -o "$(call OBJNAME,$(1))"
endef

$(foreach s,$(C_SRCS),$(eval $(call RULE_C,$(s))))
$(eval $(call RULE_S,$(STARTUP)))

$(OBJ):
	@mkdir -p "$(OBJ)"

SPEED ?= 6000

# OpenOCD resolves "source" statements relative to the working directory, so the
# program runs from its own directory when a full path is given.
OPENOCD_DIR := $(dir $(OPENOCD))
ifneq ($(strip $(OPENOCD_DIR)),)
CD_OPENOCD := cd "$(OPENOCD_DIR)" &&
else
CD_OPENOCD :=
endif

# Build and program through the on-board WCH-LinkE (SDI interface).
flash: $(HEX)
	@echo FLASH $(abspath $(HEX))
	@$(CD_OPENOCD) "$(OPENOCD)" -f "$(OPENOCD_CFG)" \
		-c "adapter speed $(SPEED)" -c "init" -c "halt" \
		-c "program {$(abspath $(HEX))} verify reset exit"

clean:
	@rm -rf "$(OUT)"

help:
	@echo "Targets:"
	@echo "  all     build $(ELF), $(HEX) and $(BIN) (default)"
	@echo "  size    print section sizes of $(ELF)"
	@echo "  flash   program $(HEX) through OpenOCD / WCH-LinkE"
	@echo "  clean   remove $(OUT)/"
	@echo ""
	@echo "Variables:"
	@echo "  MRS_ROOT       MounRiver Studio 3 install directory"
	@echo "  CROSS          toolchain prefix (default $(CROSS))"
	@echo "  TOOLCHAIN_BIN  directory holding the toolchain"
	@echo "  OPENOCD        OpenOCD executable"
	@echo "  OPENOCD_CFG    OpenOCD board/interface config (wch-riscv.cfg)"
	@echo "  SPEED          adapter speed in kHz (default $(SPEED))"

-include $(DEPS)
