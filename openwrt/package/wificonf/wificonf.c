/*
 * Wireless Network Adapter configuration utility
 *
 * Copyright (C) 2005 Felix Fietkau <nbd@vd-s.ath.cx>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <unistd.h>
#include <iwlib.h>
#include <bcmnvram.h>
#include <shutils.h>
#include <wlioctl.h>
#include <signal.h>

#define WD_INTERVAL 5
#define WD_AUTH_IDLE 20
#define WD_CLIENT_IDLE 20

/*------------------------------------------------------------------*/
/*
 * Macro to handle errors when setting WE
 * Print a nice error message and exit...
 * We define them as macro so that "return" do the right thing.
 * The "do {...} while(0)" is a standard trick
 */
#define ERR_SET_EXT(rname, request) \
	fprintf(stderr, "Error for wireless request \"%s\" (%X) :\n", \
		rname, request)

#define ABORT_ARG_NUM(rname, request) \
	do { \
		ERR_SET_EXT(rname, request); \
		fprintf(stderr, "    too few arguments.\n"); \
	} while(0)

#define ABORT_ARG_TYPE(rname, request, arg) \
	do { \
		ERR_SET_EXT(rname, request); \
		fprintf(stderr, "    invalid argument \"%s\".\n", arg); \
	} while(0)

#define ABORT_ARG_SIZE(rname, request, max) \
	do { \
		ERR_SET_EXT(rname, request); \
		fprintf(stderr, "    argument too big (max %d)\n", max); \
	} while(0)

/*------------------------------------------------------------------*/
/*
 * Wrapper to push some Wireless Parameter in the driver
 * Use standard wrapper and add pretty error message if fail...
 */
#define IW_SET_EXT_ERR(skfd, ifname, request, wrq, rname) \
	do { \
	if(iw_set_ext(skfd, ifname, request, wrq) < 0) { \
		ERR_SET_EXT(rname, request); \
		fprintf(stderr, "    SET failed on device %-1.16s ; %s.\n", \
			ifname, strerror(errno)); \
	} } while(0)

/*------------------------------------------------------------------*/
/*
 * Wrapper to extract some Wireless Parameter out of the driver
 * Use standard wrapper and add pretty error message if fail...
 */
#define IW_GET_EXT_ERR(skfd, ifname, request, wrq, rname) \
	do { \
	if(iw_get_ext(skfd, ifname, request, wrq) < 0) { \
		ERR_SET_EXT(rname, request); \
		fprintf(stderr, "    GET failed on device %-1.16s ; %s.\n", \
			ifname, strerror(errno)); \
	} } while(0)

static void set_wext_ssid(int skfd, char *ifname);

static char *prefix;
static char buffer[128];
static int wpa_enc = 0;

static char *wl_var(char *name)
{
	strcpy(buffer, prefix);
	strcat(buffer, name);
	return buffer;
}

static int nvram_enabled(char *name)
{
	return (nvram_match(name, "1") || nvram_match(name, "on") || nvram_match(name, "enabled") || nvram_match(name, "true") || nvram_match(name, "yes") ? 1 : 0);
}

static int nvram_disabled(char *name)
{
	return (nvram_match(name, "0") || nvram_match(name, "off") || nvram_match(name, "disabled") || nvram_match(name, "false") || nvram_match(name, "no") ? 1 : 0);
}

static int bcom_ioctl(int skfd, char *ifname, int cmd, void *buf, int len)
{
	struct ifreq ifr;
	wl_ioctl_t ioc;
	int ret;
	
	ioc.cmd = cmd;
	ioc.buf = buf;
	ioc.len = len;
	
	ifr.ifr_data = (caddr_t) &ioc;
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ);

	ret = ioctl(skfd, SIOCDEVPRIVATE, &ifr);

	return ret;
}

static int bcom_set_val(int skfd, char *ifname, char *var, void *val, int len)
{
	char buf[8192];
	int ret;
	
	if (strlen(var) + 1 > sizeof(buf) || len > sizeof(buf))
		return -1;

	strcpy(buf, var);
	memcpy(&buf[strlen(var) + 1], val, len);
	
	if ((ret = bcom_ioctl(skfd, ifname, WLC_SET_VAR, buf, sizeof(buf))))
		return ret;

	return 0;	
}

static int bcom_set_int(int skfd, char *ifname, char *var, int val)
{
	return bcom_set_val(skfd, ifname, var, &val, sizeof(val));
}

static void stop_bcom(int skfd, char *ifname)
{
	int val = 0;
	wlc_ssid_t ssid;
	
	if (bcom_ioctl(skfd, ifname, WLC_GET_MAGIC, &val, sizeof(val)) < 0)
		return;
	
	ssid.SSID_len = 0;
	ssid.SSID[0] = 0;
	bcom_ioctl(skfd, ifname, WLC_SET_SSID, &ssid, sizeof(ssid));
	bcom_ioctl(skfd, ifname, WLC_DOWN, NULL, 0);

}

static inline void set_distance(int skfd, char *ifname)
{
	rw_reg_t reg;
	uint32 shm;
	int val = 0;
	char *v;
	
	if (v = nvram_get(wl_var("distance"))) {
		val = strtol(v,NULL,0);
		val = 9+(val/150)+((val%150)?1:0);
		
		shm = 0x10;
		shm |= (val << 16);
		bcom_ioctl(skfd, ifname, 197, &shm, sizeof(shm));
		
		reg.byteoff = 0x684;
		reg.val = val + 510;
		reg.size = 2;
		bcom_ioctl(skfd, ifname, 102, &reg, sizeof(reg));
	}
}

static void start_bcom(int skfd, char *ifname)
{
	int val = 0;

	if (bcom_ioctl(skfd, ifname, WLC_GET_MAGIC, &val, sizeof(val)) < 0)
		return;

	bcom_ioctl(skfd, ifname, WLC_UP, &val, sizeof(val));
	set_wext_ssid(skfd, ifname);
	set_distance(skfd, ifname);
}

static int setup_bcom_wds(int skfd, char *ifname)
{
	char buf[8192];
	char wbuf[80];
	char *v;
	int wds_enabled = 0;

	memset(buf, 0, 8192);
	if (v = nvram_get(wl_var("wds"))) {
		struct maclist *wdslist = (struct maclist *) buf;
		struct ether_addr *addr = wdslist->ea;
		char *next;

		foreach(wbuf, v, next) {
			if (ether_atoe(wbuf, addr->ether_addr_octet)) {
				wdslist->count++;
				addr++;
				wds_enabled = 1;
			}
		}
	}
	bcom_ioctl(skfd, ifname, WLC_SET_WDSLIST, buf, sizeof(buf));

	return wds_enabled;
}

void start_watchdog(int skfd, char *ifname)
{
	FILE *f;
	unsigned char buf[8192], wdslist[8192], wbuf[80], *v, *p, *next, *tmp;
	int i, j, wds = 0, c = 0, restart_wds = 0, wdstimeout = 0, infra;
	int tsf_attack = 0;
	unsigned int cur_tsf = 0;

	if (fork())
		return;

	f = fopen("/var/run/wifi.pid", "w");
	fprintf(f, "%d\n", getpid());
	fclose(f);

	infra = strtol(nvram_safe_get(wl_var("infra")), NULL, 0);
	
	v = nvram_safe_get(wl_var("wds"));
	memset(wdslist, 0, 8192);
	p = wdslist;
	foreach(wbuf, v, next) {
		if (ether_atoe(wbuf, p)) {
			p += 6;
			wds++;
		}
	}
	
	for (;;) {
		sleep(1);
		
		/* refresh the distance setting - the driver might change it */
		set_distance(skfd, ifname);

		if (restart_wds)
			wdstimeout--;

		if ((c++ < WD_INTERVAL) || ((restart_wds > 0) && (wdstimeout > 0)))
			continue;
		else
			c = 0;

		/*
		 * In adhoc mode it can be desirable to use a specific BSSID to prevent
		 * accidental cell splitting caused by broken cards/drivers.
		 * When wl0_bssid is set, make sure the current BSSID matches the one
		 * set in nvram. If it doesn't change it and try to overpower the hostile
		 * AP by increasing the upper 32 bit of the TSF by one every time.
		 *
		 * In client mode simply use that variable to connect to a specific AP
		 */
		if ((infra < 1) ||
			nvram_match(wl_var("mode"), "sta") ||
			nvram_match(wl_var("mode"), "wet")) {
			rw_reg_t reg;

			if (!(tmp = nvram_get(wl_var("bssid"))))
				continue;

			if (!ether_atoe(tmp, wbuf))
				continue;

			if ((bcom_ioctl(skfd, ifname, WLC_GET_BSSID, buf, 6) < 0) ||
				(memcmp(buf, "\0\0\0\0\0\0", 6) == 0) ||
				(memcmp(buf, wbuf, 6) != 0)) {
			
				if (bcom_ioctl(skfd, ifname, (infra < 1 ? WLC_SET_BSSID : WLC_REASSOC), wbuf, 6) < 0)
					continue;

				if (infra < 1) {
					/* upper 32 bit of the TSF */
					memset(&reg, 0, sizeof(reg));
					reg.size = 4;
					reg.byteoff = 0x184;
					
					bcom_ioctl(skfd, ifname, WLC_R_REG, &reg, sizeof(reg));

					if (reg.val > cur_tsf)
						cur_tsf = reg.val;
					
					cur_tsf |= 1;
					cur_tsf <<=1;
					reg.val = (cur_tsf == ~0 ? cur_tsf : cur_tsf + 1);
					bcom_ioctl(skfd, ifname, WLC_W_REG, &reg, sizeof(reg));
					
					/* set the lower 32 bit as well */
					reg.byteoff = 0x180;
					bcom_ioctl(skfd, ifname, WLC_W_REG, &reg, sizeof(reg));
					  
					/* set the bssid again, just in case.. */
					bcom_ioctl(skfd, ifname, WLC_SET_BSSID, wbuf, 6);
					
					/* reached the maximum, next time wrap around to (1 << 16)
					 * instead of 0 */
					if (cur_tsf == ~0)
						cur_tsf = (1 << 16);
				}
			}
		}

		if (infra < 1)
			continue;

		if (nvram_match(wl_var("mode"), "sta") ||
			nvram_match(wl_var("mode"), "wet")) {

			i = 0;
			if (bcom_ioctl(skfd, ifname, WLC_GET_BSSID, buf, 6) < 0) 
				i = 1;
			if (memcmp(buf, "\0\0\0\0\0\0", 6) == 0)
				i = 1;
			
			memset(buf, 0, 8192);
			strcpy(buf, "sta_info");
			bcom_ioctl(skfd, ifname, WLC_GET_BSSID, buf + strlen(buf) + 1, 6);
			if (bcom_ioctl(skfd, ifname, WLC_GET_VAR, buf, 8192) < 0) {
				i = 1;
			} else {
				sta_info_t *sta = (sta_info_t *) (buf + 4);
				if ((sta->flags & 0x18) != 0x18) 
					i = 1;
				if (sta->idle > WD_CLIENT_IDLE)
					i = 1;
			}
			
			if (i) 
				set_wext_ssid(skfd, ifname);
		}
		
		/* wds */
		p = wdslist;
		restart_wds = 0;
		if (wdstimeout == 0)
			wdstimeout = strtol(nvram_safe_get(wl_var("wdstimeout")),NULL,0);
		
		for (i = 0; (i < wds) && !restart_wds; i++, p += 6) {
			memset(buf, 0, 8192);
			strcpy(buf, "sta_info");
			memcpy(buf + strlen(buf) + 1, p, 6);
			if (!(bcom_ioctl(skfd, ifname, WLC_GET_VAR, buf, 8192) < 0)) {
				sta_info_t *sta = (sta_info_t *) (buf + 4);
				if ((sta->flags & 0x40) == 0x40) /* this is a wds link */ { 
					if (sta->idle > wdstimeout)
						restart_wds = 1;

					/* if not authorized after WD_AUTH_IDLE seconds idletime */
					if (((sta->flags & WL_STA_AUTHO) != WL_STA_AUTHO) && (sta->idle > WD_AUTH_IDLE))
						restart_wds = 1;
				}
			}
		}

		if (restart_wds && (wdstimeout > 0)) {
			setup_bcom_wds(skfd, ifname);
		}
	}
}

static void setup_bcom(int skfd, char *ifname)
{
	int val = 0, ap;
	char buf[8192];
	char wbuf[80];
	char *v;
	int wds_enabled = 0;
	
	if (bcom_ioctl(skfd, ifname, WLC_GET_MAGIC, &val, sizeof(val)) < 0)
		return;
	
	nvram_set(wl_var("ifname"), ifname);
	
	stop_bcom(skfd, ifname);

	/* Set Country */
	strncpy(buf, nvram_safe_get(wl_var("country_code")), 4);
	buf[3] = 0;
	bcom_ioctl(skfd, ifname, WLC_SET_COUNTRY, buf, 4);
	
	val = strtol(nvram_safe_get(wl_var("txpwr")),NULL,0);
	if (val <= 0)
		val = strtol(nvram_safe_get("pa0maxpwr"),NULL,0);

	if (val)
		bcom_set_int(skfd, ifname, "qtxpower", val);
	
	/* Set other options */
	val = nvram_enabled(wl_var("lazywds"));
	wds_enabled = val;
	bcom_ioctl(skfd, ifname, WLC_SET_LAZYWDS, &val, sizeof(val));
	
	if (v = nvram_get(wl_var("frag"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_FRAG, &val, sizeof(val));
	}
	if ((val = strtol(nvram_safe_get(wl_var("rate")),NULL,0)) > 0) {
		val /= 500000;
		bcom_ioctl(skfd, ifname, WLC_SET_RATE, &val, sizeof(val));
	}
	if (v = nvram_get(wl_var("dtim"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_DTIMPRD, &val, sizeof(val));
	}
	if (v = nvram_get(wl_var("bcn"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_BCNPRD, &val, sizeof(val));
	}
	if (v = nvram_get(wl_var("rts"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_RTS, &val, sizeof(val));
	}
	if (v = nvram_get(wl_var("antdiv"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_ANTDIV, &val, sizeof(val));
	}
	if (v = nvram_get(wl_var("txant"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_TXANT, &val, sizeof(val));
	}
	
	val = nvram_enabled(wl_var("closed"));
	bcom_ioctl(skfd, ifname, WLC_SET_CLOSED, &val, sizeof(val));

	val = nvram_enabled(wl_var("ap_isolate"));
	bcom_set_int(skfd, ifname, "ap_isolate", val);

	val = nvram_enabled(wl_var("frameburst"));
	bcom_ioctl(skfd, ifname, WLC_SET_FAKEFRAG, &val, sizeof(val));

	/* Set up MAC list */
	if (nvram_match(wl_var("macmode"), "allow"))
		val = WLC_MACMODE_ALLOW;
	else if (nvram_match(wl_var("macmode"), "deny"))
		val = WLC_MACMODE_DENY;
	else
		val = WLC_MACMODE_DISABLED;

	if ((val != WLC_MACMODE_DISABLED) && (v = nvram_get(wl_var("maclist")))) {
		struct maclist *mac_list;
		struct ether_addr *addr;
		char *next;
		
		memset(buf, 0, 8192);
		mac_list = (struct maclist *) buf;
		addr = mac_list->ea;
		
		foreach(wbuf, v, next) {
			if (ether_atoe(wbuf, addr->ether_addr_octet)) {
				mac_list->count++;
				addr++;
			}
		}
		bcom_ioctl(skfd, ifname, WLC_SET_MACLIST, buf, sizeof(buf));
	} else {
		val = WLC_MACMODE_DISABLED;
	}
	bcom_ioctl(skfd, ifname, WLC_SET_MACMODE, &val, sizeof(val));

	if (ap = !nvram_match(wl_var("mode"), "sta") && !nvram_match(wl_var("mode"), "wet"))
		wds_enabled = setup_bcom_wds(skfd, ifname);

	start_watchdog(skfd, ifname);
	
	/* Set up afterburner, disabled it if WDS is enabled */
	if (wds_enabled) {
		val = ABO_OFF;
	} else {
		val = ABO_AUTO;
		if (nvram_enabled(wl_var("afterburner")))
			val = ABO_ON;
		if (nvram_disabled(wl_var("afterburner")))
			val = ABO_OFF;
	}
	
	bcom_set_val(skfd, ifname, "afterburner_override", &val, sizeof(val));
	
	/* Set up G mode */
	bcom_ioctl(skfd, ifname, WLC_GET_PHYTYPE, &val, sizeof(val));
	if (val == 2) {
		int override = WLC_G_PROTECTION_OFF;
		int control = WLC_G_PROTECTION_CTL_OFF;
		
		if (v = nvram_get(wl_var("gmode"))) 
			val = strtol(v,NULL,0);
		else
			val = 1;

		if (val > 5)
			val = 1;

		bcom_ioctl(skfd, ifname, WLC_SET_GMODE, &val, sizeof(val));
		
		if (nvram_match(wl_var("gmode_protection"), "auto")) {
			override = WLC_G_PROTECTION_AUTO;
			control = WLC_G_PROTECTION_CTL_OVERLAP;
		}
		if (nvram_enabled(wl_var("gmode_protection"))) {
			override = WLC_G_PROTECTION_ON;
			control = WLC_G_PROTECTION_CTL_OVERLAP;
		}
		bcom_ioctl(skfd, ifname, WLC_SET_GMODE_PROTECTION_CONTROL, &override, sizeof(control));
		bcom_ioctl(skfd, ifname, WLC_SET_GMODE_PROTECTION_OVERRIDE, &override, sizeof(override));

		if (val = 0) {
			if (nvram_match(wl_var("plcphdr"), "long"))
				val = WLC_PLCP_AUTO;
			else
				val = WLC_PLCP_SHORT;

			bcom_ioctl(skfd, ifname, WLC_SET_PLCPHDR, &val, sizeof(val));
		}
	}

	bcom_ioctl(skfd, ifname, WLC_UP, &val, sizeof(val));

	if (!(v = nvram_get(wl_var("akm"))))
		v = nvram_safe_get(wl_var("auth_mode"));
	
	if (strstr(v, "wpa") || strstr(v, "psk")) {
		wpa_enc = 1;

		/* Set up WPA */
		if (nvram_match(wl_var("crypto"), "tkip"))
			val = TKIP_ENABLED;
		else if (nvram_match(wl_var("crypto"), "aes"))
			val = AES_ENABLED;
		else if (nvram_match(wl_var("crypto"), "tkip+aes") || nvram_match(wl_var("crypto"), "aes+tkip"))
			val = TKIP_ENABLED | AES_ENABLED;
		else
			val = 0;
		bcom_ioctl(skfd, ifname, WLC_SET_WSEC, &val, sizeof(val));

		if (val && strstr(v, "psk")) {
			val = (strstr(v, "psk2") ? 0x84 : 0x4);
			v = nvram_safe_get(wl_var("wpa_psk"));
			if ((strlen(v) >= 8) && (strlen(v) <= 63)) {
				
				bcom_ioctl(skfd, ifname, WLC_SET_WPA_AUTH, &val, sizeof(val));
				
				if (nvram_match(wl_var("mode"), "wet")) {
					/* Enable in-driver WPA supplicant */
					wsec_pmk_t pmk;
					
					pmk.key_len = (unsigned short) strlen(v);
					pmk.flags = WSEC_PASSPHRASE;
					strcpy(pmk.key, v);
					bcom_ioctl(skfd, ifname, WLC_SET_WSEC_PMK, &pmk, sizeof(pmk));
					bcom_set_int(skfd, ifname, "sup_wpa", 1);
				}
			}
		} else  {
			val = 1;
			bcom_ioctl(skfd, ifname, WLC_SET_EAP_RESTRICT, &val, sizeof(val));
		}
	} else {
		val = 0;

		bcom_ioctl(skfd, ifname, WLC_SET_WSEC, &val, sizeof(val));
		bcom_ioctl(skfd, ifname, WLC_SET_WPA_AUTH, &val, sizeof(val));
		bcom_ioctl(skfd, ifname, WLC_SET_EAP_RESTRICT, &val, sizeof(val));
		bcom_set_int(skfd, ifname, "sup_wpa", 0);
	}

	if (v = nvram_get(wl_var("auth"))) {
		val = strtol(v,NULL,0);
		bcom_ioctl(skfd, ifname, WLC_SET_AUTH, &val, sizeof(val));
	}
}

static void set_wext_ssid(int skfd, char *ifname)
{
	char *buffer;
	char essid[IW_ESSID_MAX_SIZE + 1];
	struct iwreq wrq;

	buffer = nvram_get(wl_var("ssid"));
	
	if (!buffer || (strlen(buffer) > IW_ESSID_MAX_SIZE)) 
		buffer = "OpenWrt";

	wrq.u.essid.flags = 1;
	strcpy(essid, buffer);
	wrq.u.essid.pointer = (caddr_t) essid;
	wrq.u.essid.length = strlen(essid) + 1;
	IW_SET_EXT_ERR(skfd, ifname, SIOCSIWESSID, &wrq, "Set ESSID");
}

static void setup_wext_wep(int skfd, char *ifname)
{
	int i, keylen;
	struct iwreq wrq;
	char keystr[5];
	char *keyval;
	unsigned char key[IW_ENCODING_TOKEN_MAX];
	
	memset(&wrq, 0, sizeof(wrq));
	strcpy(keystr, "key1");
	for (i = 1; i <= 4; i++) {
		if (keyval = nvram_get(wl_var(keystr))) {
			keylen = iw_in_key(keyval, key);
			
			if (keylen > 0) {
				wrq.u.data.length = keylen;
				wrq.u.data.pointer = (caddr_t) key;
				wrq.u.data.flags = i;
				IW_SET_EXT_ERR(skfd, ifname, SIOCSIWENCODE, &wrq, "Set Encode");
			}
		}
		keystr[3]++;
	}
	
	memset(&wrq, 0, sizeof(wrq));
	i = strtol(nvram_safe_get(wl_var("key")),NULL,0);
	if (i > 0 && i < 4) {
		wrq.u.data.flags = i | IW_ENCODE_RESTRICTED;
		IW_SET_EXT_ERR(skfd, ifname, SIOCSIWENCODE, &wrq, "Set Encode");
	}
}

static void set_wext_mode(int skfd, char *ifname)
{
	struct iwreq wrq;
	int ap = 0, infra = 0, wet = 0;
	
	/* Set operation mode */
	ap = !nvram_match(wl_var("mode"), "sta") && !nvram_match(wl_var("mode"), "wet");
	infra = !nvram_disabled(wl_var("infra"));
	wet = !ap && nvram_match(wl_var("mode"), "wet");

	wrq.u.mode = (!infra ? IW_MODE_ADHOC : (ap ? IW_MODE_MASTER : (wet ? IW_MODE_REPEAT : IW_MODE_INFRA)));
	IW_SET_EXT_ERR(skfd, ifname, SIOCSIWMODE, &wrq, "Set Mode");
}

static void setup_wext(int skfd, char *ifname)
{
	char *buffer;
	struct iwreq wrq;

	/* Set channel */
	int channel = strtol(nvram_safe_get(wl_var("channel")),NULL,0);
	
	wrq.u.freq.m = -1;
	wrq.u.freq.e = 0;
	wrq.u.freq.flags = 0;

	if (channel > 0) {
		wrq.u.freq.flags = IW_FREQ_FIXED;
		wrq.u.freq.m = channel;
		IW_SET_EXT_ERR(skfd, ifname, SIOCSIWFREQ, &wrq, "Set Frequency");
	}


	/* Disable radio if wlX_radio is set and not enabled */
	wrq.u.txpower.disabled = nvram_disabled(wl_var("radio"));

	wrq.u.txpower.value = -1;
	wrq.u.txpower.fixed = 1;
	wrq.u.txpower.flags = IW_TXPOW_DBM;
	IW_SET_EXT_ERR(skfd, ifname, SIOCSIWTXPOW, &wrq, "Set Tx Power");

	/* Set up WEP */
	if (nvram_enabled(wl_var("wep")) && !wpa_enc)
		setup_wext_wep(skfd, ifname);
	
	/* Set ESSID */
	set_wext_ssid(skfd, ifname);

}

static int setup_interfaces(int skfd, char *ifname, char *args[], int count)
{
	struct iwreq wrq;
	int rc;
	
	/* Avoid "Unused parameter" warning */
	args = args; count = count;
	
	if(iw_get_ext(skfd, ifname, SIOCGIWNAME, &wrq) < 0)
		return 0;

	if (strncmp(ifname, "ath", 3) == 0) {
		set_wext_mode(skfd, ifname);
		setup_wext(skfd, ifname);
	} else {
		stop_bcom(skfd, ifname);
		set_wext_mode(skfd, ifname);
		setup_bcom(skfd, ifname);
		setup_wext(skfd, ifname);
		start_bcom(skfd, ifname);
	}
	
	prefix[2]++;
}

int main(int argc, char **argv)
{
	int skfd;
	if((skfd = iw_sockets_open()) < 0) {
		perror("socket");
		exit(-1);
	}

	system("kill $(cat /var/run/wifi.pid 2>&-) 2>&- >&-");

	prefix = strdup("wl0_");
	iw_enum_devices(skfd, &setup_interfaces, NULL, 0);
	
	return 0;
}
