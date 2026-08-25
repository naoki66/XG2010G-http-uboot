// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Christian Marangi <ansuelsmth@gmail.com>
 */

#include <asm/gpio.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dt-bindings/gpio/gpio.h>
#include <dm/device.h>
#include <dm/ofnode.h>
#include <env.h>
#include <fdt_support.h>
#include <mtd.h>
#include <net-common.h>
#include <ubi_uboot.h>
#include <xg2010g_version.h>
#include <linux/bitops.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/kconfig.h>
#include <linux/string.h>

DECLARE_GLOBAL_DATA_PTR;

#define XG2010G_CHIP_SCU_BASE		0x1fa20000
#define XG2010G_GPIO_SYSCTL_BASE	0x1fbf0200

#define XG2010G_REG_GPIO_CTRL		0x0000
#define XG2010G_REG_GPIO_DATA		0x0004
#define XG2010G_REG_GPIO_OE		0x0014
#define XG2010G_REG_GPIO_CTRL1		0x0020
#define XG2010G_REG_GPIO_FLASH_MODE_CFG	0x0034
#define XG2010G_REG_GPIO_CTRL2		0x0060
#define XG2010G_REG_GPIO_CTRL3		0x0064
#define XG2010G_REG_GPIO_DATA1		0x0070
#define XG2010G_REG_GPIO_OE1		0x0078

#define XG2010G_DSD_PART		"dsd"
#define XG2010G_FACTORY_ENV_PART	"uenv"
#define XG2010G_UBI_PART		"system"
#define XG2010G_FACTORY_VOL		"factory"
#define XG2010G_DSD_ENV_SIZE		0x4000
#define XG2010G_FACTORY_WAN_MAC_OFFSET	0x5000
#define XG2010G_FACTORY_LAN_MAC_OFFSET	0x6000
#define XG2010G_FACTORY_SIZE		(XG2010G_FACTORY_LAN_MAC_OFFSET + ARP_HLEN)
#define XG2010G_FACTORY_BOOTARG_MAX	32
#define XG2010G_FACTORY_BOOTARG_VALUE_MAX	192

/* Keep only factory values that are boot parameters for the current FIT.
 * In particular, do not carry bootcmd/root or recovery/network state from
 * the vendor environment into the new UBI boot flow. */
static const char *const xg2010g_factory_bootarg_names[] = {
	"sdram_conf", "vendor_name", "product_name", "ubi.mtd",
	"ethaddr", "snmp_sysobjid", "country_code", "ether_gpio",
	"power_gpio", "dsl_gpio", "internet_gpio", "multi_upgrade_gpio",
	"onu_type", "qdma_init", "console", "bootflag", "serdes_sel",
	"serdes_pon", "serdes_ethernet", "serdes_wifi1", "serdes_wifi2",
	"serdes_usb1", "serdes_usb2",
};

struct xg2010g_factory_bootarg {
	const char *name;
	char value[XG2010G_FACTORY_BOOTARG_VALUE_MAX];
};

static struct xg2010g_factory_bootarg
	xg2010g_factory_bootargs[XG2010G_FACTORY_BOOTARG_MAX];
static size_t xg2010g_factory_bootarg_count;

struct xg2010g_ubi_layout {
	const char *version;
	const char *part;
};

static const struct xg2010g_ubi_layout xg2010g_ubi_layouts[] = {
	{ "2.0", XG2010G_UBI_PART },
	{ "1.5", "ubi1.5" },
	{ "1.0", "ubi1.0" },
};

static const struct xg2010g_ubi_layout *xg2010g_active_ubi_layout =
	&xg2010g_ubi_layouts[0];
static bool xg2010g_ubi_layout_probed;
static bool xg2010g_ubi_layout_available;

static const char *const xg2010g_fdt_lan_mac_paths[] = {
	"/soc/ethernet@1fb50000/ethernet@1",
	"/soc/ethernet@1fb50000/ethernet@4",
};

static const char *const xg2010g_fdt_wan_mac_paths[] = {
	"/soc/ethernet@1fb50000/ethernet@2",
};

static bool xg2010g_is_compatible(void)
{
	return of_machine_is_compatible("econet,xg2010g") ||
	       of_machine_is_compatible("econet,xg2010g-ubi") ||
	       of_machine_is_compatible("gemtek,xg2010g") ||
	       of_machine_is_compatible("gemtek,xg2010g-ubi");
}

const char *an7581_release_version(void)
{
	return XG2010G_RELEASE_VERSION;
}

const char *an7581_release_credit(void)
{
	return XG2010G_RELEASE_CREDIT;
}

static void xg2010g_clrsetbits_le32(uintptr_t addr, u32 clear, u32 set)
{
	u32 val = readl((void __iomem *)addr);

	val &= ~clear;
	val |= set;
	writel(val, (void __iomem *)addr);
}

static uintptr_t xg2010g_gpio_data_reg(u32 gpio)
{
	return XG2010G_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? XG2010G_REG_GPIO_DATA : XG2010G_REG_GPIO_DATA1);
}

static uintptr_t xg2010g_gpio_oe_reg(u32 gpio)
{
	return XG2010G_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? XG2010G_REG_GPIO_OE : XG2010G_REG_GPIO_OE1);
}

static uintptr_t xg2010g_gpio_dir_reg(u32 gpio)
{
	static const u16 dir_regs[] = {
		XG2010G_REG_GPIO_CTRL,
		XG2010G_REG_GPIO_CTRL1,
		XG2010G_REG_GPIO_CTRL2,
		XG2010G_REG_GPIO_CTRL3,
	};

	return XG2010G_GPIO_SYSCTL_BASE + dir_regs[gpio / 16];
}

static void xg2010g_gpio_direction_input(u32 gpio)
{
	u32 bank_bit = BIT(gpio % 32);
	u32 dir_bit = BIT(2 * (gpio % 16));

	xg2010g_clrsetbits_le32(xg2010g_gpio_oe_reg(gpio), bank_bit, 0);
	xg2010g_clrsetbits_le32(xg2010g_gpio_dir_reg(gpio), dir_bit, 0);
}

static void xg2010g_gpio_prepare_input(u32 gpio)
{
	/*
	 * GPIO0..GPIO15 share the flash/PWM mux register. Clear the bit to
	 * force the pin back to GPIO before sampling the button.
	 */
	if (gpio < 16)
		xg2010g_clrsetbits_le32(XG2010G_CHIP_SCU_BASE +
					XG2010G_REG_GPIO_FLASH_MODE_CFG,
					BIT(gpio), 0);

	xg2010g_gpio_direction_input(gpio);
}

static int xg2010g_recovery_button_pressed_raw(ofnode root)
{
	struct ofnode_phandle_args args;
	u32 gpio, gpio_flags = 0, val;
	int ret;

	ret = ofnode_parse_phandle_with_args(root, "recovery-gpios",
					     "#gpio-cells", 0, 0, &args);
	if (ret || args.args_count < 1)
		return 0;

	gpio = args.args[0];
	if (args.args_count > 1)
		gpio_flags = args.args[1];

	xg2010g_gpio_prepare_input(gpio);

	val = readl((void __iomem *)xg2010g_gpio_data_reg(gpio));
	ret = !!(val & BIT(gpio % 32));

	return gpio_flags & GPIO_ACTIVE_LOW ? !ret : ret;
}

static int xg2010g_mtd_read_logical(struct mtd_info *mtd, loff_t logical_ofs,
				    size_t len, void *buf)
{
	u8 *dst = buf;

	if (!mtd || !mtd->erasesize || logical_ofs < 0)
		return -EINVAL;

	/* Treat the named partition as a logical stream and skip bad NAND
	 * eraseblocks, matching the vendor DSD access path. */
	for (loff_t block = 0; block < mtd->size && len;
	     block += mtd->erasesize) {
		size_t block_len = min_t(u64, mtd->erasesize,
					  mtd->size - block);
		size_t chunk, retlen = 0;
		int ret;

		ret = mtd_block_isbad(mtd, block);
		if (ret < 0)
			return ret;
		if (ret > 0)
			continue;

		if (logical_ofs >= block_len) {
			logical_ofs -= block_len;
			continue;
		}

		chunk = min(len, block_len - (size_t)logical_ofs);
		ret = mtd_read(mtd, block + logical_ofs, chunk, &retlen, dst);
		if ((ret && ret != -EUCLEAN) || retlen != chunk)
			return ret ? ret : -EIO;

		dst += chunk;
		len -= chunk;
		logical_ofs = 0;
	}

	return len ? -ENOSPC : 0;
}

static int xg2010g_read_dsd_data(size_t offset, size_t size, void *buf)
{
	struct mtd_info *mtd;
	int ret;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(XG2010G_DSD_PART);
	if (IS_ERR_OR_NULL(mtd))
		return IS_ERR(mtd) ? PTR_ERR(mtd) : -ENODEV;

	ret = xg2010g_mtd_read_logical(mtd, offset, size, buf);
	put_mtd_device(mtd);
	if (ret)
		return ret;

	return 0;
}

static int xg2010g_dsd_get_var(const char *buf, size_t len, const char *key,
				 char *value, size_t value_len)
{
	size_t key_len = strlen(key);
	const char *cur = buf;
	const char *end = buf + len;

	if (!value_len)
		return -EINVAL;

	while (cur < end) {
		const char *line_end = memchr(cur, '\n', end - cur);
		size_t line_len, copy_len;

		if (!line_end)
			line_end = end;

		line_len = line_end - cur;
		if (line_len > key_len && !memcmp(cur, key, key_len)) {
			copy_len = line_len - key_len;
			if (copy_len >= value_len)
				copy_len = value_len - 1;

			memcpy(value, cur + key_len, copy_len);
			value[copy_len] = '\0';

			while (copy_len && value[copy_len - 1] == '\r')
				value[--copy_len] = '\0';

			return 0;
		}

		if (line_end == end)
			break;
		cur = line_end + 1;
	}

	return -ENOENT;
}

static int xg2010g_get_dsd_ethaddrs(u8 *lan_mac, u8 *wan_mac)
{
	char *buf;
	char lan_str[ARP_HLEN_ASCII + 1];
	char wan_str[ARP_HLEN_ASCII + 1];
	int ret;

	buf = malloc(XG2010G_DSD_ENV_SIZE + 1);
	if (!buf)
		return -ENOMEM;

	ret = xg2010g_read_dsd_data(0, XG2010G_DSD_ENV_SIZE, buf);
	if (ret)
		goto out;

	buf[XG2010G_DSD_ENV_SIZE] = '\0';

	ret = xg2010g_dsd_get_var(buf, XG2010G_DSD_ENV_SIZE, "lan_mac=",
				  lan_str, sizeof(lan_str));
	if (ret)
		goto out;

	ret = xg2010g_dsd_get_var(buf, XG2010G_DSD_ENV_SIZE, "wan_mac=",
				  wan_str, sizeof(wan_str));
	if (ret)
		goto out;

	string_to_enetaddr(lan_str, lan_mac);
	string_to_enetaddr(wan_str, wan_mac);
	if (!is_valid_ethaddr(lan_mac) || !is_valid_ethaddr(wan_mac)) {
		ret = -EINVAL;
		goto out;
	}

	ret = 0;

out:
	free(buf);
	return ret;
}

static int xg2010g_factory_env_get(const u8 *env, size_t env_len,
				   const char *key, char *value,
				   size_t value_len)
{
	const u8 *data;
	const u8 *end;
	size_t key_len;

	if (!env || env_len <= sizeof(u32) || !key || !value || !value_len)
		return -EINVAL;

	data = env + sizeof(u32);
	end = env + env_len;
	key_len = strlen(key);

	while (data < end && *data) {
		const u8 *entry_end = memchr(data, '\0', end - data);
		const u8 *equals;
		size_t copy_len;

		if (!entry_end)
			break;
		equals = memchr(data, '=', entry_end - data);
		if (equals && equals - data == key_len &&
		    !memcmp(data, key, key_len)) {
			copy_len = entry_end - equals - 1;
			if (!copy_len || copy_len >= value_len)
				return -EINVAL;
			for (size_t i = 0; i < copy_len; i++)
				if (data[equals - data + 1 + i] < 0x20 ||
				    data[equals - data + 1 + i] > 0x7e)
					return -EINVAL;
			memcpy(value, equals + 1, copy_len);
			value[copy_len] = '\0';
			return 0;
		}
		data = entry_end + 1;
	}

	return -ENOENT;
}

/* Read the compatible production boot parameters without importing vendor
 * commands, temporary network state, or the legacy mtdblock root setting. */
static void xg2010g_load_factory_bootargs(void)
{
	struct mtd_info *mtd;
	u8 *buf;
	u32 stored_crc;
	size_t retlen;
	size_t i;
	int ret;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(XG2010G_FACTORY_ENV_PART);
	if (IS_ERR_OR_NULL(mtd)) {
		printf("XG2010G: factory env partition '%s' unavailable: %d\n",
		       XG2010G_FACTORY_ENV_PART,
		       IS_ERR(mtd) ? (int)PTR_ERR(mtd) : -ENODEV);
		return;
	}
	if (mtd->size < CONFIG_ENV_SIZE) {
		printf("XG2010G: factory env partition too small (%llu)\n",
		       (unsigned long long)mtd->size);
		goto out_put;
	}

	buf = malloc(CONFIG_ENV_SIZE);
	if (!buf) {
		printf("XG2010G: unable to allocate factory env buffer\n");
		goto out_put;
	}

	/* The factory env is at offset zero of uenv.  Read it directly first;
	 * this matches fw_env.config and avoids changing offsets on good blocks. */
	ret = mtd_read(mtd, 0, CONFIG_ENV_SIZE, &retlen, buf);
	if ((ret && ret != -EUCLEAN) || retlen != CONFIG_ENV_SIZE) {
		ret = xg2010g_mtd_read_logical(mtd, 0, CONFIG_ENV_SIZE, buf);
		if (ret) {
			printf("XG2010G: factory env read failed: %d\n", ret);
			goto out_free;
		}
	}

	memcpy(&stored_crc, buf, sizeof(stored_crc));
	if (stored_crc != crc32(0, buf + sizeof(u32),
				CONFIG_ENV_SIZE - sizeof(u32)))
		printf("XG2010G: factory env CRC mismatch; validating entries\n");

	xg2010g_factory_bootarg_count = 0;
	for (i = 0; i < ARRAY_SIZE(xg2010g_factory_bootarg_names) &&
	     i < XG2010G_FACTORY_BOOTARG_MAX; i++) {
		char *value = xg2010g_factory_bootargs[
			xg2010g_factory_bootarg_count].value;

		if (xg2010g_factory_env_get(buf, CONFIG_ENV_SIZE,
					    xg2010g_factory_bootarg_names[i], value,
					    XG2010G_FACTORY_BOOTARG_VALUE_MAX))
			continue;
		xg2010g_factory_bootargs[xg2010g_factory_bootarg_count].name =
			xg2010g_factory_bootarg_names[i];
		xg2010g_factory_bootarg_count++;
	}

	printf("XG2010G: loaded %zu compatible factory boot parameters\n",
	       xg2010g_factory_bootarg_count);

out_free:
	free(buf);
out_put:
	put_mtd_device(mtd);
}

static int xg2010g_create_ubi_volume(const char *name, size_t size)
{
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = size;
	req.vol_type = UBI_STATIC_VOLUME;
	req.name_len = strlen(name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);

	return ret;
}

static int xg2010g_resize_ubi_volume(const char *name, size_t size)
{
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	int ret, needed_pebs;

	if (!name || !*name)
		return -EINVAL;

	desc = ubi_open_volume_nm(0, name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	needed_pebs = DIV_ROUND_UP(size, vol->usable_leb_size);

	mutex_lock(&vol->ubi->device_mutex);
	ret = ubi_resize_volume(desc, needed_pebs);
	mutex_unlock(&vol->ubi->device_mutex);
	ubi_close_volume(desc);

	return ret;
}

static int xg2010g_ensure_ubi_volume(const char *name, size_t size)
{
	struct ubi_volume_desc *desc;
	unsigned long long cur_size;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	desc = ubi_open_volume_nm(0, name, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc)) {
		ret = xg2010g_create_ubi_volume(name, size);
		if (!ret)
			return 0;
		return ret;
	}

	cur_size = (unsigned long long)desc->vol->reserved_pebs *
		   (unsigned long long)desc->vol->usable_leb_size;
	ubi_close_volume(desc);

	if (size <= cur_size)
		return 0;

	return xg2010g_resize_ubi_volume(name, size);
}

static const struct xg2010g_ubi_layout *
xg2010g_find_ubi_layout(const char *part)
{
	int i;

	if (!part)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(xg2010g_ubi_layouts); i++) {
		if (!strcmp(part, xg2010g_ubi_layouts[i].part))
			return &xg2010g_ubi_layouts[i];
	}

	return NULL;
}

static int xg2010g_select_ubi(const char *part)
{
	struct ubi_device *ubi;
	bool selected = false;

	if (!xg2010g_find_ubi_layout(part))
		return -EINVAL;

	ubi = ubi_get_device(0);
	if (ubi) {
		selected = ubi->mtd && !strcmp(ubi->mtd->name, part);
		ubi_put_device(ubi);
	}

	return selected ? 0 : ubi_part(part, NULL);
}

const char *xg2010g_detect_ubi_part(void)
{
	if (xg2010g_ubi_layout_probed)
		return xg2010g_active_ubi_layout->part;

	/*
	 * The vendor kernel exposes the persistent partition as "system"; there are
	 * no legacy 1.5/1.0 views to probe.
	 */
	xg2010g_ubi_layout_probed = true;
	xg2010g_ubi_layout_available = true;
	xg2010g_active_ubi_layout = &xg2010g_ubi_layouts[0];
	printf("XG2010G: detected UBI %s on '%s'\n",
	       xg2010g_active_ubi_layout->version,
	       xg2010g_active_ubi_layout->part);

	return xg2010g_active_ubi_layout->part;
}

const char *xg2010g_detect_ubi_version(void)
{
	xg2010g_detect_ubi_part();
	return xg2010g_ubi_layout_available ?
		xg2010g_active_ubi_layout->version : "unformatted";
}

const char *env_ubi_get_part(void)
{
	return xg2010g_detect_ubi_part();
}

int xg2010g_sync_factory_part(const char *part)
{
	u8 *src = NULL, *dst = NULL;
	u8 *wan_mac, *lan_mac;
	bool same = false;
	int ret = 0;

	if (!xg2010g_is_compatible())
		return 0;

	ret = xg2010g_select_ubi(part);
	if (ret) {
		printf("XG2010G: skipping factory sync; UBI is unavailable: %d\n",
		       ret);
		return ret;
	}

	src = malloc(XG2010G_FACTORY_SIZE);
	dst = malloc(XG2010G_FACTORY_SIZE);
	if (!src || !dst) {
		ret = -ENOMEM;
		goto out;
	}

	memset(src, 0xff, XG2010G_FACTORY_SIZE);
	wan_mac = src + XG2010G_FACTORY_WAN_MAC_OFFSET;
	lan_mac = src + XG2010G_FACTORY_LAN_MAC_OFFSET;

	ret = xg2010g_get_dsd_ethaddrs(lan_mac, wan_mac);
	if (ret) {
		printf("XG2010G: failed to read MAC addresses from DSD: %d\n",
		       ret);
		goto out;
	}

	ret = ubi_volume_read(XG2010G_FACTORY_VOL, (char *)dst, 0,
			      XG2010G_FACTORY_SIZE);
	if (!ret && !memcmp(src, dst, XG2010G_FACTORY_SIZE))
		same = true;

	if (same) {
		ret = 0;
		goto out;
	}

	ret = xg2010g_ensure_ubi_volume(XG2010G_FACTORY_VOL,
					XG2010G_FACTORY_SIZE);
	if (ret) {
		printf("XG2010G: failed to prepare UBI volume '%s': %d\n",
		       XG2010G_FACTORY_VOL, ret);
		goto out;
	}

	ret = ubi_volume_write(XG2010G_FACTORY_VOL, src, 0,
			       XG2010G_FACTORY_SIZE);
	if (ret) {
		printf("XG2010G: failed to update UBI volume '%s': %d\n",
		       XG2010G_FACTORY_VOL, ret);
		goto out;
	}

	printf("XG2010G: synchronized %u bytes of DSD factory data to '%s'\n",
	       XG2010G_FACTORY_SIZE, XG2010G_FACTORY_VOL);

out:
	free(src);
	free(dst);
	return ret;
}

int xg2010g_sync_factory(void)
{
	const char *part = xg2010g_detect_ubi_part();

	if (!xg2010g_ubi_layout_available)
		return -ENODEV;

	return xg2010g_sync_factory_part(part);
}

static void xg2010g_mac_add(const u8 *base, u8 delta, u8 *mac)
{
	int i;
	unsigned int carry = delta;

	memcpy(mac, base, ARP_HLEN);
	for (i = ARP_HLEN - 1; i >= 0 && carry; i--) {
		carry += mac[i];
		mac[i] = carry & 0xff;
		carry >>= 8;
	}
}

static int xg2010g_get_runtime_ethaddrs(u8 *lan_mac, u8 *wan_mac)
{
	int ret;

	ret = xg2010g_get_dsd_ethaddrs(lan_mac, wan_mac);
	if (!ret)
		return 0;

	if (!eth_env_get_enetaddr("ethaddr", lan_mac) ||
	    !is_valid_ethaddr(lan_mac))
		return ret;

	if (!eth_env_get_enetaddr("eth1addr", wan_mac) ||
	    !is_valid_ethaddr(wan_mac))
		xg2010g_mac_add(lan_mac, 4, wan_mac);

	return 0;
}

static void xg2010g_sync_runtime_ethaddrs(void)
{
	u8 lan_mac[ARP_HLEN], wan_mac[ARP_HLEN];
	int ret;

	if (!xg2010g_is_compatible())
		return;

	ret = xg2010g_get_dsd_ethaddrs(lan_mac, wan_mac);
	if (ret) {
		printf("XG2010G: failed to read runtime MACs from DSD: %d\n",
		       ret);
		return;
	}

	eth_env_set_enetaddr("ethaddr", lan_mac);
	eth_env_set_enetaddr("eth1addr", wan_mac);
	printf("XG2010G: MACs from DSD LAN=%pM WAN=%pM\n",
	       lan_mac, wan_mac);
}

static int xg2010g_fdt_set_mac(void *blob, const char *path, const u8 *mac)
{
	int node, ret;

	node = fdt_path_offset(blob, path);
	if (node < 0)
		return node;

	ret = fdt_setprop(blob, node, "mac-address", mac, ARP_HLEN);
	if (ret)
		return ret;

	return fdt_setprop(blob, node, "local-mac-address", mac, ARP_HLEN);
}

static void xg2010g_fixup_fdt_macs(void *blob)
{
	u8 lan_mac[ARP_HLEN], wan_mac[ARP_HLEN];
	int i, ret;

	if (!xg2010g_is_compatible() && !xg2010g_is_compatible())
		return;

	if (xg2010g_get_runtime_ethaddrs(lan_mac, wan_mac))
		return;

	for (i = 0; i < ARRAY_SIZE(xg2010g_fdt_lan_mac_paths); i++) {
		ret = xg2010g_fdt_set_mac(blob, xg2010g_fdt_lan_mac_paths[i],
					  lan_mac);
		if (ret && ret != -FDT_ERR_NOTFOUND)
			printf("XG2010G: failed to update MAC for %s: %d\n",
			       xg2010g_fdt_lan_mac_paths[i], ret);
	}

	for (i = 0; i < ARRAY_SIZE(xg2010g_fdt_wan_mac_paths); i++) {
		ret = xg2010g_fdt_set_mac(blob, xg2010g_fdt_wan_mac_paths[i],
					  wan_mac);
		if (ret && ret != -FDT_ERR_NOTFOUND)
			printf("XG2010G: failed to update MAC for %s: %d\n",
			       xg2010g_fdt_wan_mac_paths[i], ret);
	}
}

/*
 * The production FIT carries a vendor bootargs string in its own DTB.
 * Keep the immutable defaults there, but replace values that are persisted
 * in U-Boot env before Linux sees the command line.
 */
static int xg2010g_replace_fdt_bootarg(void *blob, const char *name,
				       const char *value)
{
	int chosen, src_len;
	const char *src;
	const char *pos;
	char *out, *dst;
	size_t name_len, value_len, out_size;
	bool found = false;
	int ret;

	if (!name || !*name || !value || !*value)
		return 0;

	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen < 0)
		return chosen;

	src = fdt_getprop(blob, chosen, "bootargs", &src_len);
	if (!src || src_len < 0)
		return 0;

	name_len = strlen(name);
	value_len = strlen(value);
	out_size = src_len + name_len + value_len + 3;
	out = malloc(out_size);
	if (!out)
		return -ENOMEM;

	dst = out;
	pos = src;
	while (*pos) {
		const char *token;
		size_t token_len;

		while (*pos && isspace(*pos))
			pos++;
		if (!*pos)
			break;

		token = pos;
		while (*pos && !isspace(*pos))
			pos++;
		token_len = pos - token;

		if (dst != out)
			*dst++ = ' ';
		if (token_len > name_len &&
		    !memcmp(token, name, name_len) &&
		    token[name_len] == '=') {
			memcpy(dst, name, name_len);
			dst += name_len;
			*dst++ = '=';
			memcpy(dst, value, value_len);
			dst += value_len;
			found = true;
		} else {
			memcpy(dst, token, token_len);
			dst += token_len;
		}
	}

	if (!found) {
		if (dst != out)
			*dst++ = ' ';
		memcpy(dst, name, name_len);
		dst += name_len;
		*dst++ = '=';
		memcpy(dst, value, value_len);
		dst += value_len;
	}
	*dst = '\0';

	ret = fdt_setprop(blob, chosen, "bootargs", out, dst - out + 1);
	free(out);
	return ret;
}

static void xg2010g_fixup_fdt_bootargs(void *blob)
{
	u8 lan_mac[ARP_HLEN];
	char mac_str[ARP_HLEN_ASCII + 1];
	size_t i;
	int ret;

	if (!xg2010g_is_compatible())
		return;

	for (i = 0; i < xg2010g_factory_bootarg_count; i++) {
		ret = xg2010g_replace_fdt_bootarg(blob,
						  xg2010g_factory_bootargs[i].name,
						  xg2010g_factory_bootargs[i].value);
		if (ret)
			printf("XG2010G: failed to update bootargs %s: %d\n",
			       xg2010g_factory_bootargs[i].name, ret);
	}

	if (!eth_env_get_enetaddr("ethaddr", lan_mac) ||
	    !is_valid_ethaddr(lan_mac))
		return;

	snprintf(mac_str, sizeof(mac_str),
		 "%02x:%02x:%02x:%02x:%02x:%02x",
		 lan_mac[0], lan_mac[1], lan_mac[2], lan_mac[3], lan_mac[4],
		 lan_mac[5]);
	ret = xg2010g_replace_fdt_bootarg(blob, "ethaddr", mac_str);
	if (ret)
		printf("XG2010G: failed to update bootargs ethaddr: %d\n", ret);
}

int board_init(void)
{
	/* address of boot parameters */
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;

	return 0;
}

int run_http_recovery(void);

static int xg2010g_recovery_button_pressed(void)
{
	ofnode root;

	/*
	 * The board reset key is the physical SoC GPIO0. The generic
	 * en7581 pinctrl GPIO child is exposed with gpio_offs=13, so a DT cell
	 * of <0> is not physical GPIO0 through the DM GPIO API. Prefer the
	 * board-level register path; it also restores the GPIO mux before
	 * sampling the key.
	 */
	root = ofnode_path("/");
	return xg2010g_recovery_button_pressed_raw(root);
}

int board_late_init(void)
{
	char boot_ubi[64];
	const char *ubi_part;
	const char *bootcmd;
	ulong recovery_addr;

	/*
	 * A blank/corrupt recovery-env must not prevent access to the console.
	 * Keep board-late initialization out of the recovery path until the
	 * built-in defaults have been saved to mtd4/recovery-env.
	 */
	if (gd->env_valid != ENV_VALID) {
		/* Initialise the dedicated recovery-env partition automatically. */
		if (env_save())
			printf("XG2010G: recovery-env initialization failed; console only\n");
		else
			printf("XG2010G: initialized recovery-env with default environment\n");
		/* Continue board initialization so DSD MACs are applied immediately. */
	}

	printf("XG2010G release %s - %s\n",
	       XG2010G_RELEASE_VERSION, XG2010G_RELEASE_CREDIT);
	printf("XG2010G HTTP recovery: type 'http_recovery', then open http://192.168.255.1/ (PC 192.168.255.2/24)\n");

	/* Populate the Ethernet addresses before the network stack is initialized. */
	/*
	 * Factory MACs are not guaranteed to be present in the boot environment.
	 * Read them from dsd before
	 * eth_initialize() runs, otherwise the Ethernet uclass generates a random
	 * locally-administered address. This is a small 4 KiB read and is safe
	 * after initr_nand.
	 */
	xg2010g_sync_runtime_ethaddrs();
	/*
	 * The production environment lives in uenv (mtd1), while recovery
	 * settings live in recovery-env (mtd4). Import only the mode selectors
	 * needed by the production FIT; never merge the vendor environment into
	 * the recovery environment.
	 */
	xg2010g_load_factory_bootargs();
	ubi_part = xg2010g_detect_ubi_part();
	snprintf(boot_ubi, sizeof(boot_ubi),
		 "ubi part %s && run boot_production", ubi_part);
	env_set("boot_ubi", boot_ubi);
	/*
	 * The persistent environment may come from an older image whose
	 * bootcmd starts with the removed legacy 'flash' command (for example
	 * the factory 'flash imgread 2048;bootm' recipe). Once that
	 * command fails, the rest of the old recipe can try to boot the
	 * chainloader FIT left at loadaddr instead of the production FIT.
	 * Normalize only these known legacy recipes; preserve user commands.
	 */
	bootcmd = env_get("bootcmd");
	if (!bootcmd || !strncmp(bootcmd, "flash ", 6) ||
	    strstr(bootcmd, "http_recovery")) {
		env_set("bootcmd", "run boot_ubi");
		printf("XG2010G: normalized legacy bootcmd to direct UBI FIT\n");
	}
	/* Older persistent environments may also lack the helper recipes that
	 * the current boot_ubi command invokes. Restore only missing/legacy
	 * definitions so the production FIT can be booted without erasing env. */
	if (!env_get("ubi_read_production"))
		env_set("ubi_read_production", "ubi read ${loadaddr} fit");
	if (!env_get("bootconf")) {
		env_set("bootconf", "config-1");
		printf("XG2010G: defaulted missing bootconf to config-1\n");
	}
	bootcmd = env_get("boot_production");
	if (!bootcmd || !strncmp(bootcmd, "flash ", 6))
		env_set("boot_production",
		 "run ubi_read_production && bootm ${loadaddr}#${bootconf}");
	/* The factory environment carries fdt_high=0xac000000 from the
	 * vendor boot flow. That fixed ceiling forces the relocated DTB into
	 * an unsuitable high-memory window on this 64-bit U-Boot. Let the
	 * normal LMB allocator choose a valid, reserved-safe address instead. */
	if (env_get("fdt_high")) {
		env_set("fdt_high", NULL);
		printf("XG2010G: cleared legacy fdt_high override\n");
	}
	/*
	 * The factory environment may carry a legacy bootargs value with
	 * root=/dev/fit0, tclinux_info and ubi.block.  U-Boot gives that
	 * environment string precedence over the FIT DTB, which makes the
	 * ramdisk mount successfully and then panics while looking for fit0.
	 * Recovery images keep the complete command line in the FIT DTB, so
	 * discard the stale environment override on every boot.
	 */
	if (env_get("bootargs")) {
		env_set("bootargs", NULL);
		printf("XG2010G: cleared legacy bootargs override\n");
	}
	/*
	 * Do not perform large raw NAND/UBI reads from board_late_init.  On this
	 * board the first-stage loader may leave SNFI/DMA active; large early
	 * reads can overwrite relocated U-Boot text before eth_initialize().
	 * Factory synchronisation is deferred to the normal production path.
	 */
	/* Recovery variables, including recovery_trigger, come only from mtd4/recovery-env. */
	if (env_get("recovery_trigger") &&
	    !strcmp(env_get("recovery_trigger"), "1")) {
		env_set("recovery_trigger", "0");
		if (env_save())
			printf("XG2010G: failed to clear recovery trigger\n");
		printf("Recovery trigger consumed, starting web recovery...\n");
	} else if (!xg2010g_recovery_button_pressed()) {
		return 0;
	} else {
		printf("Recovery button detected, starting web recovery...\n");
	}

	env_set("ipaddr", "192.168.255.1");
	env_set("netmask", "255.255.255.0");
	env_set("gatewayip", "0.0.0.0");
	printf("XG2010G recovery network: port=%s rtl8261_patch_autoload=%s\n",
	       env_get("recovery_port") ? env_get("recovery_port") : "auto",
	       env_get("rtl8261_patch_autoload") ?
	       env_get("rtl8261_patch_autoload") : "board-default-on");

	/*
	 * Keep the recovery upload buffer well away from the low-memory
	 * boot/load addresses. Large HTTP uploads are staged fully in RAM
	 * before flashing.
	 */
	recovery_addr = gd->ram_base + 0x10000000UL;
	if ((recovery_addr < gd->ram_base) ||
	    (recovery_addr >= gd->ram_base + gd->ram_size))
		recovery_addr = CONFIG_SYS_LOAD_ADDR;
	env_set_hex("recovery_addr", recovery_addr);

	if (IS_ENABLED(CONFIG_HTTPD_RECOVERY)) {
		/* Do not fall through into autoboot after Ctrl-C. Re-entering the
		 * chainloader FIT while Ethernet/QDMA is being torn down can abort. */
		run_http_recovery();
		env_set("bootdelay", "-1");
		printf("Recovery stopped; autoboot disabled (reset to resume normal boot)\n");
	} else
		printf("HTTP recovery is not enabled.\n");

	return 0;
}

#if defined(CONFIG_OF_LIBFDT) && defined(CONFIG_OF_BOARD_SETUP)
int ft_board_setup(void *blob, struct bd_info *bd)
{
	if (!blob)
		return 0;

	xg2010g_fixup_fdt_macs(blob);
	xg2010g_fixup_fdt_bootargs(blob);

	return 0;
}
#endif
