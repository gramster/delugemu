# SPDX-License-Identifier: GPL-2.0-or-later
#
# delugemu device models, compiled into QEMU's arm-softmmu target.
#
# This file is pulled into the QEMU build by a `subdir('deluge')` line that
# scripts/integrate.sh appends to qemu/hw/meson.build. That line sits at the end
# of hw/meson.build, after `subdir('arm')`, so the Arm board source set
# `arm_common_ss` is already defined and in scope here. (Meson `subdir()` shares
# the caller's variable scope.)
#
# Our public headers are exposed to the rest of the tree by symlinking them into
# qemu/include/hw/<category>/ (also done by integrate.sh), so no include-path
# manipulation is needed here -- `#include "hw/arm/rza1l_soc.h"` just works.
#
# Sources are gated on CONFIG_DELUGE (defined in this directory's Kconfig).
# arm_common_ss is the arch-specific source set used by all Arm board/SoC models
# (boot.c, the bcm/stellaris/etc. boards), registered via hw_common_arch['arm'].

arm_common_ss.add(when: 'CONFIG_DELUGE', if_true: files(
  # Board + SoC
  'hw/arm/deluge.c',
  'hw/arm/rza1l_soc.c',
  # Peripherals
  'hw/char/rza1l_scif.c',
  'hw/display/deluge_oled.c',
  'hw/display/deluge_skin.c',
  'hw/display/deluge_padgrid.c',
  'hw/input/deluge_input.c',
  'hw/misc/deluge_pic.c',
  'hw/misc/rza1l_cpg.c',
  'hw/misc/rza1l_wdt.c',
  'hw/misc/rza1l_bsc.c',
  'hw/misc/rza1l_adc.c',
  'hw/misc/rza1l_rtc.c',
  'hw/gpio/rza1l_gpio.c',
  'hw/ssi/rza1l_rspi.c',
  'hw/ssi/rza1l_spibsc.c',
  'hw/ssi/rza1l_ssif.c',
  'hw/timer/rza1l_mtu2.c',
  'hw/timer/rza1l_ostm.c',
  'hw/dma/rza1l_dmac.c',
  'hw/sd/rza1l_sdhi.c',
  'hw/usb/rza1l_usb.c',
))
