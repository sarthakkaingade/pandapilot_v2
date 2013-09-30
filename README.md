Pandapilot_v2
1. Go to the pandapilot_v2 directory and undergo below commands
2.~/pandapilot_v2/Firmware$ make clean
3.~/pandapilot_v2/Firmware$ make distclean
4.~/pandapilot_v2/Firmware$ make archives
5.~/pandapilot_v2/Firmware$ make all

After doing this on Terminal, The last lines of compilation output are as below:
ROMFS: romfs.img
OBJ: romfs.o
CC: romfs.o.c
LINK: /path/to/pandapilot_v2/Firmware/Build/navstik-v1_default.build/firmware.elf
BIN: /path/to/pandapilot_v2/Firmware/Build/navstik-v1_default.build/firmware.bin
Generating /path/to/pandapilot_v2/Firmware/Build/navstik-v1_default.build/firmware.ns
make[1]: Leaving directory ` /path/to/pandapilot_v2/Firmware/Build/navstik-v1_default.build'
%% Copying /path/to/pandapilot_v2/Firmware/Images/navstik-v1_default.ns

6.Download bin file by execuing below command:

sudo dfu-util --device 0483:df11 -a0 --dfuse-address 0x8000000 -D Build/navstik-v1_default.build/firmware.bin


For more instructions, Visit NavStik wiki page: wiki.navstik.org
