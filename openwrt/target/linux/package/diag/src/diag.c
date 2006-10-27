/*
 * diag.c - GPIO interface driver for Broadcom boards
 *
 * Copyright (C) 2006 Mike Baker <mbm@openwrt.org>,
 *                    Felix Fietkau <nbd@openwrt.org>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Id$
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kmod.h>
#include <linux/proc_fs.h>
#include <linux/moduleparam.h>
#include <asm/uaccess.h>

#include <osl.h>
#include <bcmdevs.h>
#include <sbutils.h>
#include <sbconfig.h>
#include <sbchipc.h>

#define MODULE_NAME "diag"

#undef AUTO
#define MAX_GPIO 8

//#define DEBUG
//#define BUTTON_READ

static unsigned int gpiomask = 0;
module_param(gpiomask, int, 0644);

enum polarity_t {
	AUTO = 0,
	NORMAL = 1,
	REVERSE = 2,
};

enum {
	PROC_BUTTON,
	PROC_LED,
	PROC_MODEL,
	PROC_GPIOMASK
};

struct prochandler_t {
	int type;
	void *ptr;
};


struct button_t {
	struct prochandler_t proc;
	char *name;
	u16 gpio;
	u8 polarity;
	u8 pressed;
	unsigned long seen;
};

struct led_t {
	struct prochandler_t proc;
	char *name;
	u16 gpio;
	u8 polarity;
};

struct platform_t {
	char *name;
	struct button_t buttons[MAX_GPIO];
	struct led_t leds[MAX_GPIO];
};

enum {
	/* Linksys */
	WAP54GV1,
	WAP54GV3,
	WRT54GV1,
	WRT54G,
	WRTSL54GS,
	WRT54G3G,
	
	/* ASUS */
	WLHDD,
	WL300G,
	WL500G,
	WL500GD,
	WL500GP,
	ASUS_4702,
	
	/* Buffalo */
	WBR2_G54,
	WHR_G54S,
	WHR_HP_G54,
	WLA2_G54L,
	BUFFALO_UNKNOWN,
	BUFFALO_UNKNOWN_4710,

	/* Siemens */
	SE505V1,
	SE505V2,
	
	/* US Robotics */
	USR5461,

	/* Dell */
	TM2300,

	/* Motorola */
	WR850GV1,
	WR850GV2,
};

static struct platform_t platforms[] = {
	/* Linksys */
	[WAP54GV1] = {
		.name		= "Linksys WAP54G V1",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 0 },
		},
		.leds		= { 
			{ .name = "diag", .gpio = 1 << 3 },
			{ .name = "wlan", .gpio = 1 << 4 },
		},
	},
	[WAP54GV3] = {
		.name		= "Linksys WAP54G V3",
		.buttons	= {
			/* FIXME: verify this */
			{ .name = "reset", .gpio = 1 << 7 },
			{ .name = "ses", .gpio = 1 << 0 },
		},
		.leds		= { 
			/* FIXME: diag? */
			{ .name = "ses", .gpio = 1 << 1 },
		},
	},
	[WRT54GV1] = {
		.name		= "Linksys WRT54G V1.x",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
		},
		.leds		= { 
			/* FIXME */
		},
	},
	[WRT54G] = {
		.name		= "Linksys WRT54G(S)",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
			{ .name = "ses", .gpio = 1 << 4 }
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 1, .polarity = NORMAL },
			{ .name = "dmz", .gpio = 1 << 7 },
			{ .name = "ses_white", .gpio = 1 << 2 },
			{ .name = "ses_orange", .gpio = 1 << 3 },
		},
	},
	[WRTSL54GS] = {
		.name		= "Linksys WRTSL54GS",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
			{ .name = "ses", .gpio = 1 << 4 }
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 1, .polarity = NORMAL },
			{ .name = "dmz", .gpio = 1 << 7 },
			{ .name = "ses_white", .gpio = 1 << 5 },
			{ .name = "ses_orange", .gpio = 1 << 7 },
		},
	},
	[WRT54G3G] = {
		.name		= "Linksys WRT54G3G",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
			{ .name = "3g", .gpio = 1 << 4 }
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 1, .polarity = NORMAL },
			{ .name = "dmz", .gpio = 1 << 7 },
			{ .name = "3g_green", .gpio = 1 << 2, .polarity = NORMAL },
			{ .name = "3g_blue", .gpio = 1 << 3, .polarity = NORMAL },
			{ .name = "3g_blink", .gpio = 1 << 5, .polarity = NORMAL },
		},
	},
	/* Asus */
	[WLHDD] = {
		.name		= "ASUS WL-HDD",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 0 },
		},
	},
	[WL300G] = {
		.name		= "ASUS WL-500g",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 0 },
		},
	},
	[WL500G] = {
		.name		= "ASUS WL-500g",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 0 },
		},
	},
	[WL500GD] = {
		.name		= "ASUS WL-500g Deluxe",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 0 },
		},
	},
	[WL500GP] = {
		.name		= "ASUS WL-500g Premium",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 0 },
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 1, .polarity = NORMAL },
			{ .name = "ses", .gpio = 1 << 4 },
		},
	},
	[ASUS_4702] = {
		.name		= "ASUS (unknown, BCM4702)",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 6 },
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 0 },
		},
	},
	/* Buffalo */
	[WHR_G54S] = {
		.name		= "Buffalo WHR-G54S",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 4 },
			{ .name = "ses", .gpio = 1 << 0 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
			{ .name = "internal", .gpio = 1 << 3 },
			{ .name = "ses", .gpio = 1 << 6 },
		},
	},
	[WBR2_G54] = {
		.name		= "Buffalo WBR2-G54",
		/* FIXME: verify */
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 7 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
		},
	},
	[WHR_HP_G54] = {
		.name		= "Buffalo WHR-HP-G54",
		/* FIXME: verify */
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 4 },
			{ .name = "ses", .gpio = 1 << 0 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
			{ .name = "internal", .gpio = 1 << 3 },
			{ .name = "ses", .gpio = 1 << 6 },
		},
	},
	[WLA2_G54L] = {
		.name		= "Buffalo WLA2-G54L",
		/* FIXME: verify */
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 7 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
		},
	},
	[BUFFALO_UNKNOWN] = {
		.name		= "Buffalo (unknown)",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 7 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
		},
	},
	[BUFFALO_UNKNOWN_4710] = {
		.name		= "Buffalo (unknown, BCM4710)",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 4 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
		},
	},
	/* Siemens */
	[SE505V1] = {
		.name		= "Siemens SE505 V1",
		.buttons	= {
			/* No usable buttons */
		},
		.leds		= {
			{ .name = "dmz", .gpio = 1 << 4 },
			{ .name = "wlan", .gpio = 1 << 3 },
		},
	},
	[SE505V2] = {
		.name		= "Siemens SE505 V2",
		.buttons	= {
			/* No usable buttons */
		},
		.leds		= {
			{ .name = "power", .gpio = 1 << 5 },
			{ .name = "dmz", .gpio = 1 << 0 },
			{ .name = "wlan", .gpio = 1 << 3 },
		},
	},
	/* US Robotics */
	[USR5461] = {
		.name		= "U.S. Robotics USR5461",
		.buttons	= {
			/* No usable buttons */
		},
		.leds		= {
			{ .name = "wlan", .gpio = 1 << 0 },
			{ .name = "printer", .gpio = 1 << 1 },
		},
	},
	/* Dell */
	[TM2300] = {
		.name		= "Dell TrueMobile 2300",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 0 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 7 },
		},
	},
	/* Motorola */
	[WR850GV1] = {
		.name		= "Motorola WR850G V1",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 0 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 3 },
			{ .name = "wlan_red", .gpio = 1 << 5, .polarity = NORMAL },
			{ .name = "wlan_green", .gpio = 1 << 7, .polarity = NORMAL },
		},
	},
	[WR850GV2] = {
		.name		= "Motorola WR850G V2",
		.buttons	= {
			{ .name = "reset", .gpio = 1 << 5 },
		},
		.leds		= {
			{ .name = "diag", .gpio = 1 << 1 },
			{ .name = "wlan", .gpio = 1 << 0 },
			{ .name = "modem", .gpio = 1 << 7, .polarity = NORMAL },
		},
	},
};

static struct proc_dir_entry *diag, *leds;

extern void *bcm947xx_sbh;
#define sbh bcm947xx_sbh

static int sb_irq(void *sbh);
static struct platform_t *platform;

#include <linux/tqueue.h>
static struct tq_struct tq;
extern char *nvram_get(char *str);

static inline char *getvar(char *str)
{
	return nvram_get(str)?:"";
}

static struct platform_t *platform_detect(void)
{
	char *boardnum, *boardtype, *buf;

	boardnum = getvar("boardnum");
	boardtype = getvar("boardtype");
	if (strncmp(getvar("pmon_ver"), "CFE", 3) == 0) {
		/* CFE based - newer hardware */
		if (!strcmp(boardnum, "42")) { /* Linksys */
			if (!strcmp(boardtype, "0x0101"))
				return &platforms[WRT54G3G];

			if (!strcmp(getvar("et1phyaddr"),"5") && !strcmp(getvar("et1mdcport"), "1"))
				return &platforms[WRTSL54GS];
			
			/* default to WRT54G */
			return &platforms[WRT54G];
		}
		
		if (!strcmp(boardnum, "45")) { /* ASUS */
			if (!strcmp(boardtype,"0x042f"))
				return &platforms[WL500GP];
			else
				return &platforms[WL500GD];
		}
		
		if (!strcmp(boardnum, "10496"))
			return &platforms[USR5461];
	} else { /* PMON based - old stuff */
		if (!strncmp(boardtype, "bcm94710dev", 11)) {
			if (!strcmp(boardtype, "42"))
				return &platforms[WRT54GV1];
			if (simple_strtoul(boardnum, NULL, 9) == 2)
				return &platforms[WAP54GV1];
		}
		if (!strncmp(getvar("hardware_version"), "WL500-", 6))
			return &platforms[WL500G];
		if (!strncmp(getvar("hardware_version"), "WL300-", 6)) {
			/* Either WL-300g or WL-HDD, do more extensive checks */
			if ((simple_strtoul(getvar("et0phyaddr"), NULL, 0) == 0) &&
				(simple_strtoul(getvar("et1phyaddr"), NULL, 9) == 1))
				return &platforms[WLHDD];
			if ((simple_strtoul(getvar("et0phyaddr"), NULL, 0) == 0) &&
				(simple_strtoul(getvar("et1phyaddr"), NULL, 9) == 10))
				return &platforms[WL300G];
		}

		/* unknown asus stuff, probably bcm4702 */
		if (!strncmp(boardnum, "asusX", 5))
			return &platforms[ASUS_4702];

		if ((simple_strtoul(getvar("GemtekPmonVer"), NULL, 0) == 9) &&
			(simple_strtoul(getvar("et0phyaddr"), NULL, 0) == 30))
			return &platforms[WR850GV1];
	}

	if ((buf = (nvram_get("melco_id") ?: nvram_get("buffalo_id")))) {
		/* Buffalo hardware, check id for specific hardware matches */
		if (!strcmp(buf, "29bb0332"))
			return &platforms[WBR2_G54];
		if (!strcmp(buf, "29129"))
			return &platforms[WLA2_G54L];
		if (!strcmp(buf, "30189"))
			return &platforms[WHR_HP_G54];
		if (!strcmp(buf, "30182"))
			return &platforms[WHR_G54S];
	}

	if (buf || !strcmp(boardnum, "00")) {/* probably buffalo */
		if (!strncmp(boardtype, "bcm94710ap", 10))
			return &platforms[BUFFALO_UNKNOWN_4710];
		else
			return &platforms[BUFFALO_UNKNOWN];
	}

	if (!strcmp(getvar("CFEver"), "MotoWRv203"))
		return &platforms[WR850GV2];

	/* not found */
	return NULL;
}

static ssize_t diag_proc_read(struct file *file, char *buf, size_t count, loff_t *ppos);
static ssize_t diag_proc_write(struct file *file, const char *buf, size_t count, void *data);
static struct file_operations diag_proc_fops = {
	read: diag_proc_read,
	write: diag_proc_write
};



static ssize_t diag_proc_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
#ifdef LINUX_2_4
	struct inode *inode = file->f_dentry->d_inode;
	struct proc_dir_entry *dent = inode->u.generic_ip;
#else
	struct proc_dir_entry *dent = PDE(file->f_dentry->d_inode);
#endif
	char *page;
	int len = 0;
	
	if ((page = kmalloc(1024, GFP_KERNEL)) == NULL)
		return -ENOBUFS;
	
	if (dent->data != NULL) {
		struct prochandler_t *handler = (struct prochandler_t *) dent->data;
		switch (handler->type) {
#ifdef BUTTON_READ
			case PROC_BUTTON: {
				struct button_t * button = (struct button_t *) handler->ptr;
				len = sprintf(page, "%d\n", button->pressed);
			}
#endif
			case PROC_LED: {
				struct led_t * led = (struct led_t *) handler->ptr;
				int in = (sb_gpioin(sbh) & led->gpio ? 1 : 0);
				int p = (led->polarity == NORMAL ? 0 : 1);
				len = sprintf(page, "%d\n", ((in ^ p) ? 1 : 0));
				break;
			}
			case PROC_MODEL:
				len = sprintf(page, "%s\n", platform->name);
				break;
			case PROC_GPIOMASK:
				len = sprintf(page, "%d\n", gpiomask);
				break;
		}
	}
	len += 1;

	if (*ppos < len) {
		len = min_t(int, len - *ppos, count);
		if (copy_to_user(buf, (page + *ppos), len)) {
			kfree(page);
			return -EFAULT;
		}
		*ppos += len;
	} else {
		len = 0;
	}

	return len;
}


static ssize_t diag_proc_write(struct file *file, const char *buf, size_t count, void *data)
{
#ifdef LINUX_2_4
	struct inode *inode = file->f_dentry->d_inode;
	struct proc_dir_entry *dent = inode->u.generic_ip;
#else
	struct proc_dir_entry *dent = PDE(file->f_dentry->d_inode);
#endif
	char *page;
	int ret = -EINVAL;

	if ((page = kmalloc(count + 1, GFP_KERNEL)) == NULL)
		return -ENOBUFS;

	if (copy_from_user(page, buf, count)) {
		kfree(page);
		return -EINVAL;
	}
	page[count] = 0;
	
	if (dent->data != NULL) {
		struct prochandler_t *handler = (struct prochandler_t *) dent->data;
		switch (handler->type) {
			case PROC_LED: {
				struct led_t *led = (struct led_t *) handler->ptr;
				int p = (led->polarity == NORMAL ? 0 : 1);
				
				if (led->gpio & gpiomask)
					break;
				sb_gpiocontrol(sbh, led->gpio, 0);
				sb_gpioouten(sbh, led->gpio, led->gpio);
				sb_gpioout(sbh, led->gpio, ((p ^ (page[0] == '1')) ? led->gpio : 0));
				break;
			}
			case PROC_GPIOMASK:
				gpiomask = simple_strtoul(page, NULL, 16);
				break;
		}
		ret = count;
	}

	kfree(page);
	return ret;
}

static void hotplug_button(struct button_t *b)
{
	char *argv [3], *envp[6], *buf, *scratch;
	int i;
	
	if (!hotplug_path[0])
		return;

	if (in_interrupt()) {
		printk(MODULE_NAME ": HOTPLUG WHILE IN IRQ!\n");
		return;
	}

	if (!(buf = kmalloc (256, GFP_KERNEL)))
		return;

	scratch = buf;

	i = 0;
	argv[i++] = hotplug_path;
	argv[i++] = "button";
	argv[i] = 0;

	i = 0;
	envp[i++] = "HOME=/";
	envp[i++] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
	envp[i++] = scratch;
	scratch += sprintf (scratch, "ACTION=%s", b->pressed?"pressed":"released") + 1;
	envp[i++] = scratch;
	scratch += sprintf (scratch, "BUTTON=%s", b->name) + 1;
	envp[i++] = scratch;
	scratch += sprintf (scratch, "SEEN=%ld", (jiffies - b->seen)/HZ) + 1;
	envp[i] = 0;

	call_usermodehelper (argv [0], argv, envp);
	kfree (buf);
}

static void button_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	
	struct button_t *b;
	int in = sb_gpioin(sbh);

	for (b = platform->buttons; b->name; b++) { 
		if (b->gpio & gpiomask)
			continue;
		if (b->polarity != (in & b->gpio)) {
			/* ASAP */
			b->polarity ^= b->gpio;
			sb_gpiointpolarity(sbh, b->gpio, b->polarity);

			b->pressed ^= 1;
#ifdef DEBUG
			printk(MODULE_NAME ": button: %s pressed: %d seen: %ld\n",b->name, b->pressed, (jiffies - b->seen)/HZ);
#endif
			INIT_TQUEUE(&tq, (void *)(void *)hotplug_button, (void *)b);
			schedule_task(&tq);

			b->seen = jiffies;
		}
	}
}

static void register_buttons(struct button_t *b)
{
	int irq = sb_irq(sbh) + 2;
	chipcregs_t *cc;

#ifdef BUTTON_READ
	struct proc_dir_entry *p;

	buttons = proc_mkdir("button", diag);
	if (!buttons)
		return;
#endif
	
	request_irq(irq, button_handler, SA_SHIRQ | SA_SAMPLE_RANDOM, "gpio", button_handler);

	for (; b->name; b++) {
		if (b->gpio & gpiomask)
			continue;
		sb_gpioouten(sbh,b->gpio,0);
		sb_gpiocontrol(sbh,b->gpio,0);
		b->polarity = sb_gpioin(sbh) & b->gpio;
		sb_gpiointpolarity(sbh, b->gpio, b->polarity);
		sb_gpiointmask(sbh, b->gpio,b->gpio);
#ifdef BUTTON_READ
		if ((p = create_proc_entry(b->name, S_IRUSR, buttons))) {
			b->proc.type = PROC_BUTTON;
			b->proc.ptr = (void *) b;
			p->data = (void *) &b->proc;
			p->proc_fops = &diag_proc_fops;
		}
#endif
	}

	if ((cc = sb_setcore(sbh, SB_CC, 0))) {
		int intmask;

		intmask = readl(&cc->intmask);
		intmask |= CI_GPIO;
		writel(intmask, &cc->intmask);
	}
}

static void unregister_buttons(struct button_t *b)
{
	int irq = sb_irq(sbh) + 2;

	for (; b->name; b++) {
		sb_gpiointmask(sbh, b->gpio, 0);
#ifdef BUTTON_READ
		remove_proc_entry(b->name, buttons);
#endif
	}
#ifdef BUTTON_READ
	remove_proc_entry("buttons", diag);
#endif

	free_irq(irq, button_handler);
}

static void register_leds(struct led_t *l)
{
	struct proc_dir_entry *p;

	leds = proc_mkdir("led", diag);
	if (!leds) 
		return;

	for(; l->name; l++) {
		sb_gpiointmask(sbh, l->gpio, 0);
		if ((p = create_proc_entry(l->name, S_IRUSR, leds))) {
			l->proc.type = PROC_LED;
			l->proc.ptr = l;
			p->data = (void *) &l->proc;
			p->proc_fops = &diag_proc_fops;
		}
	}
}

static void unregister_leds(struct led_t *l)
{
	for(; l->name; l++) {
		remove_proc_entry(l->name, leds);
	}
	remove_proc_entry("led", diag);
}

static void __exit diag_exit(void)
{
	if (platform->buttons)
		unregister_buttons(platform->buttons);
	if (platform->leds)
		unregister_leds(platform->leds);
	remove_proc_entry("model", diag);
	remove_proc_entry("gpiomask", diag);
	remove_proc_entry("diag", NULL);
}

static struct prochandler_t proc_model = { .type = PROC_MODEL };
static struct prochandler_t proc_gpiomask = { .type = PROC_GPIOMASK };

static int __init diag_init(void)
{
	static struct proc_dir_entry *p;

	platform = platform_detect();
	if (!platform) {
		printk(MODULE_NAME ": Router model not detected.\n");
		return -ENODEV;
	}
	printk(MODULE_NAME ": Detected '%s'\n", platform->name);

	if (!(diag = proc_mkdir("diag", NULL))) {
		printk(MODULE_NAME ": proc_mkdir on /proc/diag failed\n");
		return -EINVAL;
	}
	if ((p = create_proc_entry("model", S_IRUSR, diag))) {
		p->data = (void *) &proc_model;
		p->proc_fops = &diag_proc_fops;
	}
	if ((p = create_proc_entry("gpiomask", S_IRUSR | S_IWUSR, diag))) {
		p->data = (void *) &proc_gpiomask;
		p->proc_fops = &diag_proc_fops;
	}

	if (platform->buttons)
		register_buttons(platform->buttons);

	if (platform->leds)
		register_leds(platform->leds);

	return 0;
}

EXPORT_NO_SYMBOLS;

module_init(diag_init);
module_exit(diag_exit);

MODULE_AUTHOR("Mike Baker, Felix Fietkau / OpenWrt.org");
MODULE_LICENSE("GPL");


/* TODO: export existing sb_irq instead */
static int sb_irq(void *sbh)
{
	uint idx;
	void *regs;
	sbconfig_t *sb;
	uint32 flag, sbipsflag;
	uint irq = 0;

	regs = sb_coreregs(sbh);
	sb = (sbconfig_t *)((ulong) regs + SBCONFIGOFF);
	flag = (R_REG(&sb->sbtpsflag) & SBTPS_NUM0_MASK);

	idx = sb_coreidx(sbh);

	if ((regs = sb_setcore(sbh, SB_MIPS, 0)) ||
	    (regs = sb_setcore(sbh, SB_MIPS33, 0))) {
		sb = (sbconfig_t *)((ulong) regs + SBCONFIGOFF);

		/* sbipsflag specifies which core is routed to interrupts 1 to 4 */
		sbipsflag = R_REG(&sb->sbipsflag);
		for (irq = 1; irq <= 4; irq++, sbipsflag >>= 8) {
			if ((sbipsflag & 0x3f) == flag)
				break;
		}
		if (irq == 5)
			irq = 0;
	}

	sb_setcoreidx(sbh, idx);

	return irq;
}
