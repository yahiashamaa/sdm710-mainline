# SPDX-License-Identifier: GPL-2.0-only

obj-y				+= ioctl.o common.o

obj-$(CONFIG_ETHTOOL_NETLINK)	+= ethtool_nl.o

ethtool_nl-y	:= netlink.o bitset.o strset.o linkinfo.o linkmodes.o rss.o \
		   linkstate.o debug.o wol.o features.o privflags.o rings.o \
		   channels.o coalesce.o pause.o eee.o tsinfo.o cabletest.o \
		   tunnels.o fec.o eeprom.o stats.o phc_vclocks.o module.o \
		   pse-pd.o plca.o
