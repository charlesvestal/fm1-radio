#!/bin/sh
# The link probe, in the toolchain's container. Prints what the link cannot resolve.
#   probe/build.sh [extra .a ...]
set -e
cd "$(dirname "$0")/.."
S=/sdk; T=/opt/jieli/pi32v2/bin
INC="-I$S/include_lib -I$S/include_lib/driver -I$S/include_lib/driver/device -I$S/include_lib/driver/cpu/wl82
 -I$S/include_lib/system -I$S/include_lib/system/generic -I$S/include_lib/btctrler -I$S/include_lib/btctrler/port/wl82
 -I$S/include_lib/update -I$S/include_lib/btstack/third_party/common -I$S/include_lib/btstack/third_party/rcsp
 -I$S/include_lib/utils -I$S/include_lib/utils/syscfg -I$S/include_lib/utils/event -I$S/include_lib/media
 -I$S/include_lib/media/cpu/wl82 -I$S/apps -I$S/apps/demo/demo_ble/include -I$S/apps/common -I$S/apps/common/include
 -I$S/apps/common/config/include -I$S/include_lib/btstack -I$S/include_lib/net -I$S/apps/common/ble/include
 -I$S/include_lib/newlib/include -Ifw/ble/shim"
CF="-target pi32v2 -integrated-as -mcpu=r3 -mfprev1 -fno-common -Oz -flto -fallow-pointer-null -fprefer-gnu-section
 -femulated-tls -fms-extensions -w -DSUPPORT_MS_EXTENSIONS -DCONFIG_RELEASE_ENABLE -DCONFIG_CPU_WL82
 -DCONFIG_FREE_RTOS_ENABLE -DCONFIG_THREAD_ENABLE -D__GCC_PI32V2__ -DCONFIG_NEW_ECC_ENABLE -DCONFIG_SFC_ENABLE
 -DCONFIG_NO_SDRAM_ENABLE -DCONFIG_BT_ENABLE=1"
SRC="probe/probe.c probe/platform_stub.c fw/ble/shim/os.c fw/ble/shim/systimer.c fw/ble/shim/misc.c fw/ble/shim/rf_const.c $S/apps/common/ble/le_trans_data.c $S/apps/common/config/bt_profile_config.c
 $S/apps/common/config/log_config/lib_btctrler_config.c $S/apps/common/config/log_config/lib_btstack_config.c"
L=$S/cpu/wl82/liba
INC=$(echo $INC); CF=$(echo $CF); SRC=$(echo $SRC)
LIBS="--start-group $L/btctrler.a $L/btstack.a $L/crypto_toolbox_Osize.a $L/lib_ccm_aes.a ${*:-} --end-group"
docker run --rm --platform linux/amd64 -v "$PWD:/work" -v "$HOME/fw-AC79_AIoT_SDK:/sdk:ro" \
  -v "$HOME/.jieli/toolchain:/opt/jieli:ro" -w /work debian:bookworm-slim sh -c "
  mkdir -p build/probe && rm -f build/probe/*.o
  $T/clang -target pi32v2 -integrated-as -c fw/ble/shim/ctx.S -o build/probe/ctx.o || exit 1
  for f in $SRC; do $T/clang $CF $INC -c \$f -o build/probe/\$(basename \$f .c).o || exit 1; done
  $T/ld --plugin $T/LLVMgold.so -o build/probe/probe.elf build/probe/*.o --plugin-opt=mcpu=r3 -e probe_entry --gc-sections \
     --unresolved-symbols=report-all -noinhibit-exec $LIBS 2> build/probe/link.log; echo link rc \$?"
grep -o "undefined reference to \`[^']*'" build/probe/link.log | sed "s/undefined reference to \`//;s/'//" | sort -u > build/probe/undefined.txt
wc -l < build/probe/undefined.txt
