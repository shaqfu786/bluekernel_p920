/*
 * drivers/mmc/host/omap_hsmmc.c
 *
 * Driver for OMAP2430/3430 MMC controller.
 *
 * Copyright (C) 2007 Texas Instruments.
 *
 * Authors:
 *	Syed Mohammed Khasim	<x0khasim@ti.com>
 *	Madhusudhan		<madhu.cr@ti.com>
 *	Mohit Jalori		<mjalori@ti.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/clk.h>
#include <linux/mmc/host.h>
#include <linux/mmc/core.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/io.h>
#include <linux/semaphore.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>

#include <plat/dma.h>
#include <mach/hardware.h>
#include <plat/board.h>
#include <plat/mmc.h>
#include <plat/cpu.h>
#include <plat/omap-pm.h>

#ifdef CONFIG_OMAP4_DPLL_CASCADING
#include <linux/notifier.h>
#include <plat/clock.h>
#endif

/* OMAP HSMMC Host Controller Registers */
#define OMAP_HSMMC_SYSCONFIG	0x0010
#define OMAP_HSMMC_SYSSTATUS	0x0014
#define OMAP_HSMMC_CON		0x002C
#define OMAP_HSMMC_BLK		0x0104
#define OMAP_HSMMC_ARG		0x0108
#define OMAP_HSMMC_CMD		0x010C
#define OMAP_HSMMC_RSP10	0x0110
#define OMAP_HSMMC_RSP32	0x0114
#define OMAP_HSMMC_RSP54	0x0118
#define OMAP_HSMMC_RSP76	0x011C
#define OMAP_HSMMC_DATA		0x0120
#define OMAP_HSMMC_PSTATE		0x0124
#define OMAP_HSMMC_HCTL		0x0128
#define OMAP_HSMMC_SYSCTL	0x012C
#define OMAP_HSMMC_STAT		0x0130
#define OMAP_HSMMC_IE		0x0134
#define OMAP_HSMMC_ISE		0x0138
#define OMAP_HSMMC_CAPA		0x0140
#define OMAP_HSMMC_ADMA_ES	0x0154
#define OMAP_HSMMC_ADMA_SAL	0x0158

#define VS18			(1 << 26)
#define VS30			(1 << 25)
#define SDVS18			(0x5 << 9)
#define SDVS30			(0x6 << 9)
#define SDVS33			(0x7 << 9)
#define SDVS_MASK		0x00000E00
#define SDVSCLR			0xFFFFF1FF
#define SDVSDET			0x00000400
#define AUTOIDLE		0x1
#define SDBP			(1 << 8)
#define DTO			0xe
#define ICE			0x1
#define ICS			0x2
#define CEN			(1 << 2)
#define CLKD_MASK		0x0000FFC0
#define CLKD_SHIFT		6
#define DTO_MASK		0x000F0000
#define DTO_SHIFT		16
#define INT_EN_MASK		0x307F0033
#define BWR_ENABLE		(1 << 4)
#define BRR_ENABLE		(1 << 5)
#define DTO_ENABLE		(1 << 20)
#define INIT_STREAM		(1 << 1)
#define ACEN_ACMD12		(1 << 2)
#define ACEN_ACMD23		(2 << 2)
#define DP_SELECT		(1 << 21)
#define DDIR			(1 << 4)
#define DMA_EN			0x1
#define MSBS			(1 << 5)
#define BCE			(1 << 1)
#define FOUR_BIT		(1 << 1)
#define DDR			(1 << 19)
#define DW8			(1 << 5)
#define CC			0x1
#define TC			0x02
#define OD			0x1
#define ERR			(1 << 15)
#define CMD_TIMEOUT		(1 << 16)
#define DATA_TIMEOUT		(1 << 20)
#define CMD_CRC			(1 << 17)
#define DATA_CRC		(1 << 21)
#define CARD_ERR		(1 << 28)
#define STAT_CLEAR		0xFFFFFFFF
#define INIT_STREAM_CMD		0x00000000
#define DUAL_VOLT_OCR_BIT	7
#define SRC			(1 << 25)
#define SRD			(1 << 26)
#define SOFTRESET		(1 << 1)
#define RESETDONE		(1 << 0)
#define DMAS			(0x2 << 3)
#define CAPA_ADMA_SUPPORT       (1 << 19)
#define ADMA_XFER_VALID		(1 << 0)
#define ADMA_XFER_END		(1 << 1)
#define ADMA_XFER_EN_INT	(1 << 2)
#define ADMA_XFER_LINK		(1 << 4)
#define ADMA_XFER_DESC		(1 << 5)
#define DMA_MNS_ADMA_MODE	(1 << 20)
#define ADMA_ERR		(1 << 25)
#define ADMA_XFER_INT		(1 << 3)

#define DMA_TABLE_NUM_ENTRIES	1024
#define ADMA_TABLE_SZ 	\
	(DMA_TABLE_NUM_ENTRIES * sizeof(struct adma_desc_table))

#define SDMA_XFER	1
#define ADMA_XFER	2
/*
 * According to TRM, It is possible to transfer
 * upto 64KB per ADMA table entry.
 * But 64KB = 0x10000 cannot be represented
 * using a 16bit integer in 1 ADMA table row.
 * Hence rounding it to a lesser value.
 */
#define ADMA_MAX_XFER_PER_ROW (60 * 1024)
#define ADMA_MAX_BLKS_PER_ROW (ADMA_MAX_XFER_PER_ROW / 512)


#define AUTO_CMD12		(1 << 0)	/* Auto CMD12 support */
/*
 * FIXME: Most likely all the data using these _DEVID defines should come
 * from the platform_data, or implemented in controller and slot specific
 * functions.
 */
#define OMAP_MMC1_DEVID		0
#define OMAP_MMC2_DEVID		1
#define OMAP_MMC3_DEVID		2
#define OMAP_MMC4_DEVID		3
#define OMAP_MMC5_DEVID		4

#define MMC_TIMEOUT_MS		20
#define OMAP_MMC_MASTER_CLOCK	96000000
#ifdef CONFIG_OMAP4_DPLL_CASCADING
#define OMAP_MMC_DPLL_CLOCK	49152000
#endif
#define DRIVER_NAME		"omap_hsmmc"

/* Timeouts for entering power saving states on inactivity, msec */
#define OMAP_MMC_DISABLED_TIMEOUT	1
#define OMAP_MMC_SLEEP_TIMEOUT		1000
#define OMAP_MMC_OFF_TIMEOUT		8000

/*
 * One controller can have multiple slots, like on some omap boards using
 * omap.c controller driver. Luckily this is not currently done on any known
 * omap_hsmmc.c device.
 */
#define mmc_slot(host)		(host->pdata->slots[host->slot_id])

/*
 * MMC Host controller read/write API's
 */
#define OMAP_HSMMC_READ(base, reg)	\
	__raw_readl((base) + OMAP_HSMMC_##reg)

#define OMAP_HSMMC_WRITE(base, reg, val) \
	__raw_writel((val), (base) + OMAP_HSMMC_##reg)

struct adma_desc_table {
	u16 attr;
	u16 length;
	dma_addr_t addr;
};

struct omap_hsmmc_host {
	struct	device		*dev;
	struct	mmc_host	*mmc;
	struct	mmc_request	*mrq;
	struct	mmc_command	*cmd;
	struct	mmc_data	*data;
	struct	clk		*fclk;
	struct	clk		*iclk;
	struct	clk		*dbclk;
#ifdef CONFIG_OMAP4_DPLL_CASCADING
	struct notifier_block	nb;
	int			dpll_entry;
	int			dpll_exit;
	spinlock_t		dpll_lock;
#endif
	/*
	 * vcc == configured supply
	 * vcc_aux == optional
	 *   -	MMC1, supply for DAT4..DAT7
	 *   -	MMC2/MMC2, external level shifter voltage supply, for
	 *	chip (SDIO, eMMC, etc) or transceiver (MMC2 only)
	 */
	struct	regulator	*vcc;
	struct	regulator	*vcc_aux;
	struct	work_struct	mmc_carddetect_work;
	void	__iomem		*base;
	resource_size_t		mapbase;
	spinlock_t		irq_lock; /* Prevent races with irq handler */
	unsigned int		id;
	unsigned int		dma_len;
	unsigned int		dma_sg_idx;
	unsigned int		master_clock;
	unsigned char		bus_mode;
	unsigned char		power_mode;
	u32			*buffer;
	u32			bytesleft;
	int			suspended;
	int			irq;
	int			dma_type, dma_ch;
	struct adma_desc_table	*adma_table;
	dma_addr_t		phy_adma_table;
	int			dma_line_tx, dma_line_rx;
	int			slot_id;
	int			got_dbclk;
	int			response_busy;
	int			dpm_state;
	int			vdd;
	int			protect_card;
	int			reqs_blocked;
	int			use_reg;
	int			req_in_progress;
	unsigned int		flags;

	/* LGE_SJIT 2011-11-28 [dojip.kim@lge.com] Enhance the error handling */
	unsigned int		eject; /* eject state */

	struct	omap_mmc_platform_data	*pdata;
};

static void omap_hsmmc_status_notify_cb(int card_present, void *dev_id)
{
	struct omap_hsmmc_host *host = (struct omap_hsmmc_host *)dev_id;
	unsigned int status, oldstat;

	pr_debug("%s: card_present %d\n", mmc_hostname(host->mmc),
		card_present);

	if (!mmc_slot(host).mmc_data.status) {
		mmc_detect_change(host->mmc, 0);
		return;
	}

	status = mmc_slot(host).mmc_data.status(mmc_dev(host->mmc));

	oldstat = mmc_slot(host).mmc_data.card_present;
	mmc_slot(host).mmc_data.card_present = status;
	if (status ^ oldstat) {
		pr_debug("%s: Slot status change detected (%d -> %d)\n",
			mmc_hostname(host->mmc), oldstat, status);
		mmc_detect_change(host->mmc, 0);
	}
}

static int
omap_hsmmc_prepare_data(struct omap_hsmmc_host *host, struct mmc_request *req);

#ifndef CONFIG_MACH_LGE_MMC_COVER
static int omap_hsmmc_card_detect(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes card detect signal is active-low */
	return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
}

static int omap_hsmmc_get_wp(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes write protect signal is active-high */
	return gpio_get_value_cansleep(mmc->slots[0].gpio_wp);
}

static int omap_hsmmc_get_cover_state(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes card detect signal is active-low */
	return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
}
#else
static int omap_hsmmc_card_detect(struct device *dev, int slot)
{
#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//20120517 for sdcard
		static int old_status=1; // 1: inserted 0:removed
		int tmp_old_status=old_status;	
		struct omap_mmc_platform_data *mmc = dev->platform_data;
		//printk("\nsdcard status(old_status,det,cover) = (%d,%d,%d)\n",old_status,!gpio_get_value_cansleep(mmc->slots[0].switch_pin),!gpio_get_value_cansleep(mmc->slots[0].sd_cover));
	
		/* NOTE: assumes card detect signal is active-low */
		old_status = ((!gpio_get_value_cansleep(mmc->slots[0].switch_pin))&&(!gpio_get_value_cansleep(mmc->slots[0].sd_cover)));		
	
		return (((tmp_old_status)|| (!gpio_get_value_cansleep(mmc->slots[0].sd_cover)))&&(!gpio_get_value_cansleep(mmc->slots[0].switch_pin)));
#else	
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes card detect signal is active-low */
	return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
#endif
}

static int omap_hsmmc_get_wp(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes write protect signal is active-high */
	return gpio_get_value_cansleep(mmc->slots[0].gpio_wp);
}

static int omap_hsmmc_get_cover_state(struct device *dev, int slot)
{
	//struct omap_mmc_platform_data *mmc = dev->platform_data;

	/* NOTE: assumes card detect signal is active-low */
	return omap_hsmmc_card_detect(dev, 0);
}

int omap_hsmmc_get_detect_pin_state(struct omap_hsmmc_host *host)
{
	// returns 1 in case that card is IN
	// returns 0 in case that card is OUT
	//return omap_hsmmc_get_cover_state(host->dev, 0);
#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//nthyunjin.yang 120517 for sdcard
	struct omap_mmc_platform_data *mmc = host->dev->platform_data;

	return (!gpio_get_value_cansleep(mmc->slots[0].switch_pin));
#else	
	return omap_hsmmc_card_detect(host->dev, 0);
#endif	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[END]
}

#endif

#ifdef CONFIG_PM

static int omap_hsmmc_suspend_cdirq(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

	disable_irq(mmc->slots[0].card_detect_irq);
#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[START]
		disable_irq(mmc->slots[0].card_detect_irq_by_data3pin);
#endif	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[END]
	return 0;
}

static int omap_hsmmc_resume_cdirq(struct device *dev, int slot)
{
	struct omap_mmc_platform_data *mmc = dev->platform_data;

#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[START]
		enable_irq(mmc->slots[0].card_detect_irq_by_data3pin);
#endif

	enable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

#else

#define omap_hsmmc_suspend_cdirq	NULL
#define omap_hsmmc_resume_cdirq		NULL

#endif

#ifdef CONFIG_REGULATOR

static int omap_hsmmc_1_set_power(struct device *dev, int slot, int power_on,
				  int vdd)
{
	struct omap_hsmmc_host *host =
		platform_get_drvdata(to_platform_device(dev));
	int ret;

//nthyunjin.yang 120517 for sdcard
//	int pin_status = 0;
//	pin_status = omap_hsmmc_get_detect_pin_state(host);
//nthyunjin.yang 120517 for sdcard

	if (mmc_slot(host).before_set_reg)
		mmc_slot(host).before_set_reg(dev, slot, power_on, vdd);

	if (power_on)
		ret = mmc_regulator_set_ocr(host->mmc, host->vcc, vdd);
	else
		ret = mmc_regulator_set_ocr(host->mmc, host->vcc, 0);

	if (mmc_slot(host).after_set_reg)
		mmc_slot(host).after_set_reg(dev, slot, power_on, vdd);

	return ret;
}

static int omap_hsmmc_235_set_power(struct device *dev, int slot, int power_on,
				   int vdd)
{
	struct omap_hsmmc_host *host =
		platform_get_drvdata(to_platform_device(dev));
	int ret = 0;

	/*
	 * If we don't see a Vcc regulator, assume it's a fixed
	 * voltage always-on regulator.
	 */
	if (!host->vcc)
		return 0;

	if (mmc_slot(host).before_set_reg)
		mmc_slot(host).before_set_reg(dev, slot, power_on, vdd);

	/*
	 * Assume Vcc regulator is used only to power the card ... OMAP
	 * VDDS is used to power the pins, optionally with a transceiver to
	 * support cards using voltages other than VDDS (1.8V nominal).  When a
	 * transceiver is used, DAT3..7 are muxed as transceiver control pins.
	 *
	 * In some cases this regulator won't support enable/disable;
	 * e.g. it's a fixed rail for a WLAN chip.
	 *
	 * In other cases vcc_aux switches interface power.  Example, for
	 * eMMC cards it represents VccQ.  Sometimes transceivers or SDIO
	 * chips/cards need an interface voltage rail too.
	 */
	if (power_on) {
		ret = mmc_regulator_set_ocr(host->mmc, host->vcc, vdd);
		/* Enable interface voltage rail, if needed */
		if (ret == 0 && host->vcc_aux) {
			ret = regulator_enable(host->vcc_aux);
			if (ret < 0)
				ret = mmc_regulator_set_ocr(host->mmc,
							host->vcc, 0);
		}
	} else {
		/* Shut down the rail */
		if (host->vcc_aux)
			ret = regulator_disable(host->vcc_aux);
		if (!ret) {
			/* Then proceed to shut down the local regulator */
			ret = mmc_regulator_set_ocr(host->mmc,
						host->vcc, 0);
		}
	}

	if (mmc_slot(host).after_set_reg)
		mmc_slot(host).after_set_reg(dev, slot, power_on, vdd);

	return ret;
}

static int omap_hsmmc_4_set_power(struct device *dev, int slot, int power_on,
					int vdd)
{
	return 0;
}

static int omap_hsmmc_1_set_sleep(struct device *dev, int slot, int sleep,
				  int vdd, int cardsleep)
{
	struct omap_hsmmc_host *host =
		platform_get_drvdata(to_platform_device(dev));
	int mode = sleep ? REGULATOR_MODE_STANDBY : REGULATOR_MODE_NORMAL;

	return regulator_set_mode(host->vcc, mode);
}

static int omap_hsmmc_235_set_sleep(struct device *dev, int slot, int sleep,
				   int vdd, int cardsleep)
{
	struct omap_hsmmc_host *host =
		platform_get_drvdata(to_platform_device(dev));
	int err, mode;

	/*
	 * If we don't see a Vcc regulator, assume it's a fixed
	 * voltage always-on regulator.
	 */
	if (!host->vcc)
		return 0;

	mode = sleep ? REGULATOR_MODE_STANDBY : REGULATOR_MODE_NORMAL;

	if (!host->vcc_aux)
		return regulator_set_mode(host->vcc, mode);

	if (cardsleep) {
		/* VCC can be turned off if card is asleep */
		if (sleep)
			err = mmc_regulator_set_ocr(host->mmc, host->vcc, 0);
		else
			err = mmc_regulator_set_ocr(host->mmc, host->vcc, vdd);
	} else
		err = regulator_set_mode(host->vcc, mode);
	if (err)
		return err;

	if (!mmc_slot(host).vcc_aux_disable_is_sleep)
		return regulator_set_mode(host->vcc_aux, mode);

	if (sleep)
		return regulator_disable(host->vcc_aux);
	else
		return regulator_enable(host->vcc_aux);
}

static int omap_hsmmc_4_set_sleep(struct device *dev, int slot, int sleep,
					int vdd, int cardsleep)
{
	return 0;
}

static int omap_hsmmc_reg_get(struct omap_hsmmc_host *host)
{
	struct regulator *reg;
	int ret = 0;
	int ocr_value = 0;

	switch (host->id) {
	case OMAP_MMC1_DEVID:
		/* On-chip level shifting via PBIAS0/PBIAS1 */
		mmc_slot(host).set_power = omap_hsmmc_1_set_power;
		mmc_slot(host).set_sleep = omap_hsmmc_1_set_sleep;
		break;
	case OMAP_MMC2_DEVID:
	case OMAP_MMC3_DEVID:
	case OMAP_MMC5_DEVID:
		/* Off-chip level shifting, or none */
		mmc_slot(host).set_power = omap_hsmmc_235_set_power;
		mmc_slot(host).set_sleep = omap_hsmmc_235_set_sleep;
		break;
	case OMAP_MMC4_DEVID:
		mmc_slot(host).set_power = omap_hsmmc_4_set_power;
		mmc_slot(host).set_sleep = omap_hsmmc_4_set_sleep;
	default:
		pr_err("MMC%d configuration not supported!\n", host->id);
		return -EINVAL;
	}

	reg = regulator_get(host->dev, "vmmc");
	if (IS_ERR(reg)) {
		dev_dbg(host->dev, "vmmc regulator missing\n");
		/*
		* HACK: until fixed.c regulator is usable,
		* we don't require a main regulator
		* for MMC2 or MMC3
		*/
		if (host->id == OMAP_MMC1_DEVID) {
			ret = PTR_ERR(reg);
			goto err;
		}
	} else {
		host->vcc = reg;
		ocr_value = mmc_regulator_get_ocrmask(reg);
		if (!mmc_slot(host).ocr_mask) {
			mmc_slot(host).ocr_mask = ocr_value;
		} else {
			if (!(mmc_slot(host).ocr_mask & ocr_value)) {
				pr_err("MMC%d ocrmask %x is not supported\n",
					host->id, mmc_slot(host).ocr_mask);
				mmc_slot(host).ocr_mask = 0;
				return -EINVAL;
			}
		}

		/* Allow an aux regulator */
		reg = regulator_get(host->dev, "vmmc_aux");
		host->vcc_aux = IS_ERR(reg) ? NULL : reg;

		/* For eMMC do not power off when not in sleep state */
		if (mmc_slot(host).no_regulator_off_init)
			return 0;
		/*
		* UGLY HACK:  workaround regulator framework bugs.
		* When the bootloader leaves a supply active, it's
		* initialized with zero usecount ... and we can't
		* disable it without first enabling it.  Until the
		* framework is fixed, we need a workaround like this
		* (which is safe for MMC, but not in general).
		*/
		if (regulator_is_enabled(host->vcc) > 0) {
			regulator_enable(host->vcc);
			regulator_disable(host->vcc);
		}
		if (host->vcc_aux) {
			if (regulator_is_enabled(reg) > 0) {
				regulator_enable(reg);
				regulator_disable(reg);
			}
		}
	}

	return 0;

err:
	mmc_slot(host).set_power = NULL;
	mmc_slot(host).set_sleep = NULL;
	return ret;
}

static void omap_hsmmc_reg_put(struct omap_hsmmc_host *host)
{
	regulator_put(host->vcc);
	regulator_put(host->vcc_aux);
	mmc_slot(host).set_power = NULL;
	mmc_slot(host).set_sleep = NULL;
}

static inline int omap_hsmmc_have_reg(void)
{
	return 1;
}

#else

static inline int omap_hsmmc_reg_get(struct omap_hsmmc_host *host)
{
	return -EINVAL;
}

static inline void omap_hsmmc_reg_put(struct omap_hsmmc_host *host)
{
}

static inline int omap_hsmmc_have_reg(void)
{
	return 0;
}

#endif

#ifndef CONFIG_MACH_LGE_MMC_COVER //nthyunjin.yang 120517 for sdcard
static int omap_hsmmc_gpio_init(struct omap_mmc_platform_data *pdata)
{
	int ret;

	if (gpio_is_valid(pdata->slots[0].switch_pin)) {
		if (pdata->slots[0].cover)
			pdata->slots[0].get_cover_state =
					omap_hsmmc_get_cover_state;
		else
			pdata->slots[0].card_detect = omap_hsmmc_card_detect;
		pdata->slots[0].card_detect_irq =
				gpio_to_irq(pdata->slots[0].switch_pin);
		ret = gpio_request(pdata->slots[0].switch_pin, "mmc_cd");
		if (ret)
			return ret;
		ret = gpio_direction_input(pdata->slots[0].switch_pin);
		if (ret)
			goto err_free_sp;
	} else
		pdata->slots[0].switch_pin = -EINVAL;

	if (gpio_is_valid(pdata->slots[0].gpio_wp)) {
		pdata->slots[0].get_ro = omap_hsmmc_get_wp;
		ret = gpio_request(pdata->slots[0].gpio_wp, "mmc_wp");
		if (ret)
			goto err_free_cd;
		ret = gpio_direction_input(pdata->slots[0].gpio_wp);
		if (ret)
			goto err_free_wp;
	} else
		pdata->slots[0].gpio_wp = -EINVAL;

	return 0;

err_free_wp:
	gpio_free(pdata->slots[0].gpio_wp);
err_free_cd:
	if (gpio_is_valid(pdata->slots[0].switch_pin))
err_free_sp:
		gpio_free(pdata->slots[0].switch_pin);
	return ret;
}
#else
static int omap_hsmmc_gpio_init(struct omap_mmc_platform_data *pdata)
{
	int ret;

	if (gpio_is_valid(pdata->slots[0].switch_pin)) {
		if (0/*pdata->slots[0].cover*/)
			pdata->slots[0].get_cover_state =
					omap_hsmmc_get_cover_state;
		else
			pdata->slots[0].card_detect = omap_hsmmc_card_detect;
		pdata->slots[0].card_detect_irq =
				gpio_to_irq(pdata->slots[0].sd_cover/*pdata->slots[0].switch_pin*/);
		ret = gpio_request(pdata->slots[0].sd_cover/*pdata->slots[0].switch_pin*/, "mmc_cd");
		if (ret)
			return ret;
		ret = gpio_direction_input(pdata->slots[0].sd_cover/*pdata->slots[0].switch_pin*/);
		if (ret)
			goto err_free_sp;

#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[START]

		if (pdata->slots[0].cover)
		{
			pdata->slots[0].card_detect_irq_by_data3pin =
					gpio_to_irq(pdata->slots[0].switch_pin);
			ret = gpio_request(pdata->slots[0].switch_pin, "mmc_cd_byd3");
			if (ret)
				return ret;
			ret = gpio_direction_input(pdata->slots[0].switch_pin);
			if (ret)
				goto err_free_sp;
		}

#endif

	} else
		pdata->slots[0].switch_pin = -EINVAL;

	if (gpio_is_valid(pdata->slots[0].gpio_wp)) {
		pdata->slots[0].get_ro = omap_hsmmc_get_wp;
		ret = gpio_request(pdata->slots[0].gpio_wp, "mmc_wp");
		if (ret)
			goto err_free_cd;
		ret = gpio_direction_input(pdata->slots[0].gpio_wp);
		if (ret)
			goto err_free_wp;
	} else
		pdata->slots[0].gpio_wp = -EINVAL;

	return 0;

err_free_wp:
	gpio_free(pdata->slots[0].gpio_wp);
err_free_cd:
	if (gpio_is_valid(pdata->slots[0].switch_pin))
err_free_sp:
		gpio_free(pdata->slots[0].switch_pin);
	return ret;
}

#endif

static void omap_hsmmc_gpio_free(struct omap_mmc_platform_data *pdata)
{
	if (gpio_is_valid(pdata->slots[0].gpio_wp))
		gpio_free(pdata->slots[0].gpio_wp);
	if (gpio_is_valid(pdata->slots[0].switch_pin))
		gpio_free(pdata->slots[0].switch_pin);
}

/*
 * Stop clock to the card
 */
static void omap_hsmmc_stop_clock(struct omap_hsmmc_host *host)
{
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) & ~CEN);
	if ((OMAP_HSMMC_READ(host->base, SYSCTL) & CEN) != 0x0)
		dev_dbg(mmc_dev(host->mmc), "MMC Clock is not stoped\n");
}

static void omap_hsmmc_enable_irq(struct omap_hsmmc_host *host,
				  struct mmc_command *cmd)
{
	unsigned int irq_mask;

	if (host->dma_type)
		irq_mask = INT_EN_MASK & ~(BRR_ENABLE | BWR_ENABLE);
	else
		irq_mask = INT_EN_MASK;

	/* Disable timeout for erases */
	if (cmd->opcode == MMC_ERASE)
		irq_mask &= ~DTO_ENABLE;

	OMAP_HSMMC_WRITE(host->base, STAT, STAT_CLEAR);
	OMAP_HSMMC_WRITE(host->base, ISE, irq_mask);
	OMAP_HSMMC_WRITE(host->base, IE, irq_mask);
}

static void omap_hsmmc_disable_irq(struct omap_hsmmc_host *host)
{
	OMAP_HSMMC_WRITE(host->base, ISE, 0);
	OMAP_HSMMC_WRITE(host->base, IE, 0);
	OMAP_HSMMC_WRITE(host->base, STAT, STAT_CLEAR);
}

#ifdef CONFIG_PM

/*
 * Restore the MMC host context, if it was lost as result of a
 * power state change.
 */
static int omap_hsmmc_context_restore(struct omap_hsmmc_host *host)
{
	struct mmc_ios *ios = &host->mmc->ios;
	u32 hctl, capa, con, value;
	u16 dsor = 0;
	unsigned long timeout;

	if (!omap_pm_was_context_lost(host->dev))
		return 1;

	/* Wait for hardware reset */
	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, SYSSTATUS) & RESETDONE) != RESETDONE
		&& time_before(jiffies, timeout))
		;

	/* Do software reset */
	OMAP_HSMMC_WRITE(host->base, SYSCONFIG, SOFTRESET);
	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, SYSSTATUS) & RESETDONE) != RESETDONE
		&& time_before(jiffies, timeout))
		;

	OMAP_HSMMC_WRITE(host->base, SYSCONFIG,
			OMAP_HSMMC_READ(host->base, SYSCONFIG) | AUTOIDLE);

	if (host->id == OMAP_MMC1_DEVID) {
		if (host->power_mode != MMC_POWER_OFF &&
		    (1 << ios->vdd) <= MMC_VDD_23_24)
			hctl = SDVS18;
		else
			hctl = SDVS30;
		capa = VS30 | VS18;
	} else {
		hctl = SDVS18;
		capa = VS18;
	}
	if (host->dma_type == ADMA_XFER)
		hctl |= DMAS;

	OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | hctl);

	OMAP_HSMMC_WRITE(host->base, CAPA,
			OMAP_HSMMC_READ(host->base, CAPA) | capa);

	OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | SDBP);

	if (host->dma_type == ADMA_XFER) {
		value = OMAP_HSMMC_READ(host->base, CON);
		OMAP_HSMMC_WRITE(host->base, CON, value | DMA_MNS_ADMA_MODE);
	}

	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, HCTL) & SDBP) != SDBP
		&& time_before(jiffies, timeout))
		;

	omap_hsmmc_disable_irq(host);

	/* Do not initialize card-specific things if the power is off */
	if (host->power_mode == MMC_POWER_OFF)
		goto out;

	con = OMAP_HSMMC_READ(host->base, CON);
	/* configure in DDR mode */
	if (ios->timing == MMC_TIMING_UHS_DDR50) {
		con |= DDR;
	} else {
		con &= ~DDR;
	}
	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_8:
		OMAP_HSMMC_WRITE(host->base, CON, con | DW8);
		break;
	case MMC_BUS_WIDTH_4:
		OMAP_HSMMC_WRITE(host->base, CON, con & ~DW8);
		OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | FOUR_BIT);
		break;
	case MMC_BUS_WIDTH_1:
		OMAP_HSMMC_WRITE(host->base, CON, con & ~DW8);
		OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) & ~FOUR_BIT);
		break;
	}

	if (ios->clock) {
		dsor = host->master_clock / ios->clock;
		if (dsor < 1)
			dsor = 1;

		if (host->master_clock / dsor > ios->clock)
			dsor++;

		if (dsor > 250)
			dsor = 250;
	}

	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) & ~CEN);
	OMAP_HSMMC_WRITE(host->base, SYSCTL, (dsor << 6) | (DTO << 16));
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) | ICE);

	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & ICS) != ICS
		&& time_before(jiffies, timeout))
		;

	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) | CEN);

	con = OMAP_HSMMC_READ(host->base, CON);
	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		OMAP_HSMMC_WRITE(host->base, CON, con | OD);
	else
		OMAP_HSMMC_WRITE(host->base, CON, con & ~OD);
out:
	dev_dbg(mmc_dev(host->mmc), "context is restored\n");
	return 0;
}

/*
 * Save the MMC host context (store the number of power state changes so far).
 */
static void omap_hsmmc_context_save(struct omap_hsmmc_host *host)
{
	return;
}

#else

static int omap_hsmmc_context_restore(struct omap_hsmmc_host *host)
{
	return 0;
}

static void omap_hsmmc_context_save(struct omap_hsmmc_host *host)
{
}

#endif

/*
 * Send init stream sequence to card
 * before sending IDLE command
 */
static void send_init_stream(struct omap_hsmmc_host *host)
{
	int reg = 0;
	unsigned long timeout;

	if (host->protect_card)
		return;

	disable_irq(host->irq);

	OMAP_HSMMC_WRITE(host->base, IE, INT_EN_MASK);
	OMAP_HSMMC_WRITE(host->base, CON,
		OMAP_HSMMC_READ(host->base, CON) | INIT_STREAM);
	OMAP_HSMMC_WRITE(host->base, CMD, INIT_STREAM_CMD);

	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((reg != CC) && time_before(jiffies, timeout))
		reg = OMAP_HSMMC_READ(host->base, STAT) & CC;

	OMAP_HSMMC_WRITE(host->base, CON,
		OMAP_HSMMC_READ(host->base, CON) & ~INIT_STREAM);

	OMAP_HSMMC_WRITE(host->base, STAT, STAT_CLEAR);
	OMAP_HSMMC_READ(host->base, STAT);

	enable_irq(host->irq);
}

static inline
int omap_hsmmc_cover_is_closed(struct omap_hsmmc_host *host)
{
	int r = 1;

	if (mmc_slot(host).get_cover_state)
		r = mmc_slot(host).get_cover_state(host->dev, host->slot_id);
	return r;
}

/*
static ssize_t
omap_hsmmc_show_cover_switch(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mmc_host *mmc = container_of(dev, struct mmc_host, class_dev);
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	return sprintf(buf, "%s\n",
			omap_hsmmc_cover_is_closed(host) ? "closed" : "open");
}
*/

//static DEVICE_ATTR(cover_switch, S_IRUGO, omap_hsmmc_show_cover_switch, NULL);

static ssize_t
omap_hsmmc_show_slot_name(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct mmc_host *mmc = container_of(dev, struct mmc_host, class_dev);
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	return sprintf(buf, "%s\n", mmc_slot(host).name);
}

static DEVICE_ATTR(slot_name, S_IRUGO, omap_hsmmc_show_slot_name, NULL);

/*
 * Configure the response type and send the cmd.
 */
static void
omap_hsmmc_start_command(struct omap_hsmmc_host *host, struct mmc_command *cmd,
	struct mmc_data *data, bool no_autocmd12)
{
	int cmdreg = 0, resptype = 0, cmdtype = 0;

	dev_dbg(mmc_dev(host->mmc), "%s: CMD%d, argument 0x%08x\n",
		mmc_hostname(host->mmc), cmd->opcode, cmd->arg);
	host->cmd = cmd;

	omap_hsmmc_enable_irq(host, cmd);

	host->response_busy = 0;
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			resptype = 1;
		else if (cmd->flags & MMC_RSP_BUSY) {
			resptype = 3;
			host->response_busy = 1;
		} else
			resptype = 2;
	}

	/*
	 * Unlike OMAP1 controller, the cmdtype does not seem to be based on
	 * ac, bc, adtc, bcr. Only commands ending an open ended transfer need
	 * a val of 0x3, rest 0x0.
	 */
	if (cmd == host->mrq->stop)
		cmdtype = 0x3;

	cmdreg = (cmd->opcode << 24) | (resptype << 16) | (cmdtype << 22);
	if ((host->flags & AUTO_CMD12) && mmc_op_multi(cmd->opcode) &&
								!no_autocmd12)
		cmdreg |= ACEN_ACMD12;

	if (data) {
		cmdreg |= DP_SELECT | MSBS | BCE;
		if (data->flags & MMC_DATA_READ)
			cmdreg |= DDIR;
		else
			cmdreg &= ~(DDIR);
	}

	if (host->dma_type)
		cmdreg |= DMA_EN;

	host->req_in_progress = 1;

	OMAP_HSMMC_WRITE(host->base, ARG, cmd->arg);
	OMAP_HSMMC_WRITE(host->base, CMD, cmdreg);
}

static int
omap_hsmmc_get_dma_dir(struct omap_hsmmc_host *host, struct mmc_data *data)
{
	if (data->flags & MMC_DATA_WRITE)
		return DMA_TO_DEVICE;
	else
		return DMA_FROM_DEVICE;
}

static void omap_hsmmc_request_done(struct omap_hsmmc_host *host, struct mmc_request *mrq)
{
	int dma_ch;

	spin_lock(&host->irq_lock);
	host->req_in_progress = 0;
	dma_ch = host->dma_ch;
	spin_unlock(&host->irq_lock);

	omap_hsmmc_disable_irq(host);
	/* Do not complete the request if DMA is still in progress */
	if (mrq->data && host->dma_type && dma_ch != -1)
		return;
	host->mrq = NULL;
	mmc_request_done(host->mmc, mrq);
}

/*
 * Notify the transfer complete to MMC core
 */
static void
omap_hsmmc_xfer_done(struct omap_hsmmc_host *host, struct mmc_data *data)
{
	if (!data) {
		struct mmc_request *mrq = host->mrq;

		/* TC before CC from CMD6 - don't know why, but it happens */
		if (host->cmd && host->cmd->opcode == 6 &&
		    host->response_busy) {
			host->response_busy = 0;
			return;
		}

		omap_hsmmc_request_done(host, mrq);
		return;
	}

	host->data = NULL;

	if (host->dma_type == ADMA_XFER)
		dma_unmap_sg(mmc_dev(host->mmc), data->sg, host->dma_len,
					omap_hsmmc_get_dma_dir(host, data));

	if (!data->error)
		data->bytes_xfered += data->blocks * (data->blksz);
	else
		data->bytes_xfered = 0;

	if (data->stop && ((!(host->flags & AUTO_CMD12)) || data->error))
		omap_hsmmc_start_command(host, data->stop, NULL, 0);
	else {
		if (data->stop)
			data->stop->resp[0] = OMAP_HSMMC_READ(host->base,
							RSP76);
		omap_hsmmc_request_done(host, data->mrq);
	}

	return;
}

/*
 * Notify the core about command completion
 */
static void
omap_hsmmc_cmd_done(struct omap_hsmmc_host *host, struct mmc_command *cmd)
{
	int err;
	struct mmc_request *req;
	req = host->mrq;

	if ((host->mrq->sbc) && (host->cmd == host->mrq->sbc)) {
		host->cmd = NULL;
		err = omap_hsmmc_prepare_data(host, host->mrq);
		if (err) {
			req->cmd->error = err;
			if (req->data)
				req->data->error = err;
			host->mrq = NULL;
			mmc_request_done(host->mmc, req);
			return;
		}
		omap_hsmmc_start_command(host, host->mrq->cmd,
						host->mrq->data, 1);
		return;
	}

	host->cmd = NULL;

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) {
			/* response type 2 */
			cmd->resp[3] = OMAP_HSMMC_READ(host->base, RSP10);
			cmd->resp[2] = OMAP_HSMMC_READ(host->base, RSP32);
			cmd->resp[1] = OMAP_HSMMC_READ(host->base, RSP54);
			cmd->resp[0] = OMAP_HSMMC_READ(host->base, RSP76);
		} else {
			/* response types 1, 1b, 3, 4, 5, 6 */
			cmd->resp[0] = OMAP_HSMMC_READ(host->base, RSP10);
		}
	}
	if ((host->data == NULL && !host->response_busy) || cmd->error)
		omap_hsmmc_request_done(host, cmd->mrq);
}

/*
 * DMA clean up for command errors
 */
static void omap_hsmmc_dma_cleanup(struct omap_hsmmc_host *host, int errno)
{
	int dma_ch;

	host->data->error = errno;

	spin_lock(&host->irq_lock);
	dma_ch = host->dma_ch;
	host->dma_ch = -1;
	spin_unlock(&host->irq_lock);

	if ((host->dma_type == SDMA_XFER) && (dma_ch != -1)) {
		dma_unmap_sg(mmc_dev(host->mmc), host->data->sg,
			host->data->sg_len,
			omap_hsmmc_get_dma_dir(host, host->data));
		omap_free_dma(dma_ch);
	}
	host->data = NULL;
}

/*
 * Readable error output
 */
#ifdef CONFIG_MMC_DEBUG
static void omap_hsmmc_report_irq(struct omap_hsmmc_host *host, u32 status)
{
	/* --- means reserved bit without definition at documentation */
	static const char *omap_hsmmc_status_bits[] = {
		"CC", "TC", "BGE", "DMA", "BWR", "BRR", "CINS", "CREM", "CIRQ",
		"OBI", "BSR", "---", "---", "---", "---", "ERRI", "CTO", "CCRC",
		"CEB", "CIE", "DTO", "DCRC", "DEB", "CLE", "ACE", "ADMA",
		"---", "---", "CERR", "BADA", "---", "---"
	};
	char res[256];
	char *buf = res;
	int len, i;

	len = sprintf(buf, "MMC IRQ 0x%x :", status);
	buf += len;

	for (i = 0; i < ARRAY_SIZE(omap_hsmmc_status_bits); i++)
		if (status & (1 << i)) {
			len = sprintf(buf, " %s", omap_hsmmc_status_bits[i]);
			buf += len;
		}

	dev_dbg(mmc_dev(host->mmc), "%s\n", res);
}
#endif  /* CONFIG_MMC_DEBUG */

/*
 * MMC controller internal state machines reset
 *
 * Used to reset command or data internal state machines, using respectively
 *  SRC or SRD bit of SYSCTL register
 * Can be called from interrupt context
 */
static inline void omap_hsmmc_reset_controller_fsm(struct omap_hsmmc_host *host,
						   unsigned long bit)
{
	unsigned long i = 0;
	/* LGE_SJIT 2011-11-29 [dojip.kim@lge.com]
	 * Inaccurate delays when calculated based on loops_per_jiffy
	 */
	/*
	unsigned long limit = (loops_per_jiffy *
				msecs_to_jiffies(MMC_TIMEOUT_MS));
	*/

	OMAP_HSMMC_WRITE(host->base, SYSCTL,
			 OMAP_HSMMC_READ(host->base, SYSCTL) | bit);

	/*
	 * OMAP4 ES2 and greater has an updated reset logic.
	 * Monitor a 0->1 transition first
	 */
	if (mmc_slot(host).features & HSMMC_HAS_UPDATED_RESET) {
		/* LGE_SJIT 2011-11-29 [dojip.kim@lge.com]
		 * Inaccurate delays when calculated based on
		 * loops_per_jiffy
		 */
		/*
		while ((!(OMAP_HSMMC_READ(host->base, SYSCTL) & bit))
					&& (i++ < limit))
			cpu_relax();
		*/
		while ((!(OMAP_HSMMC_READ(host->base, SYSCTL) & bit))
					&& (i++ < 50))
			udelay(100);
	}
	i = 0;
	/* LGE_SJIT 2011-11-29 [dojip.kim@lge.com]
	 * Inaccurate delays when calculated based on
	 * loops_per_jiffy
	 */
	/*
	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & bit) &&
		(i++ < limit))
		cpu_relax();
	*/
	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & bit) &&
		(i++ < 50))
		udelay(100);

	if (OMAP_HSMMC_READ(host->base, SYSCTL) & bit)
		dev_err(mmc_dev(host->mmc),
			"Timeout waiting on controller reset in %s\n",
			__func__);
}

static void omap_hsmmc_do_irq(struct omap_hsmmc_host *host, int status)
{
	struct mmc_data *data;
	int end_cmd = 0, end_trans = 0;

	if (!host->req_in_progress) {
		do {
			OMAP_HSMMC_WRITE(host->base, STAT, status);
			/* Flush posted write */
			status = OMAP_HSMMC_READ(host->base, STAT);
		} while (status & INT_EN_MASK);
		return;
	}

	data = host->data;
	dev_dbg(mmc_dev(host->mmc), "IRQ Status is %x\n", status);

	if (status & ERR) {
#ifdef CONFIG_MMC_DEBUG
		omap_hsmmc_report_irq(host, status);
#endif
		if ((status & CMD_TIMEOUT) ||
			(status & CMD_CRC)) {
			if (host->cmd) {
				if (status & CMD_TIMEOUT) {
					omap_hsmmc_reset_controller_fsm(host,
									SRC);
					host->cmd->error = -ETIMEDOUT;
				} else {
					host->cmd->error = -EILSEQ;
				}
				end_cmd = 1;
			}
			if (host->data || host->response_busy) {
				if (host->data)
					omap_hsmmc_dma_cleanup(host,
								-ETIMEDOUT);
				host->response_busy = 0;
				omap_hsmmc_reset_controller_fsm(host, SRD);
			}
		}
		if ((status & DATA_TIMEOUT) ||
			(status & DATA_CRC)) {
			if (host->data || host->response_busy) {
				int err = (status & DATA_TIMEOUT) ?
						-ETIMEDOUT : -EILSEQ;

				if (host->data)
					omap_hsmmc_dma_cleanup(host, err);
				else
					host->mrq->cmd->error = err;
				host->response_busy = 0;
				omap_hsmmc_reset_controller_fsm(host, SRD);
				end_trans = 1;
			}
		}
		if (status & CARD_ERR) {
			dev_dbg(mmc_dev(host->mmc),
				"Ignoring card err CMD%d\n", host->cmd->opcode);
			if (host->cmd)
				end_cmd = 1;
			if (host->data)
				end_trans = 1;
		}
		/* LGE_SJIT_S 2011-11-29 [dojip.kim@lge.com]
		 * Detecting a 0 at the end bit position of read data
		 * when removing the sdcard
		 */
		if (status & (1 << 22)) { // DEB error
			int err = -EILSEQ;
			if (host->data)
				omap_hsmmc_dma_cleanup(host, err);
			else
				host->mrq->cmd->error = err;

			host->response_busy = 0;
			omap_hsmmc_reset_controller_fsm(host, SRD);
			end_trans = 1;
		}
		/* LGE_SJIT_E 2011-11-29 [dojip.kim@lge.com] */
		if (status & ADMA_ERR) {
			dev_dbg(mmc_dev(host->mmc),
				"ADMA err: ADMA_ES=%x, SAL=%x.\n",
					OMAP_HSMMC_READ(host->base, ADMA_ES),
					OMAP_HSMMC_READ(host->base, ADMA_SAL));
			if (host->cmd)
				end_cmd = 1;
			if (host->data)
				end_trans = 1;
		}
	}
	if (status & ADMA_XFER_INT) {
		dev_dbg(mmc_dev(host->mmc),
			"ADMA XFERINT: blk=%x at table=%x pstate=%x\n",
			OMAP_HSMMC_READ(host->base, BLK),
			OMAP_HSMMC_READ(host->base, ADMA_SAL),
			OMAP_HSMMC_READ(host->base, PSTATE));

	}

	OMAP_HSMMC_WRITE(host->base, STAT, status);

	if (end_cmd || ((status & CC) && host->cmd))
		omap_hsmmc_cmd_done(host, host->cmd);
	if ((end_trans || (status & TC)) && host->mrq)
		omap_hsmmc_xfer_done(host, data);
}

/*
 * MMC controller IRQ handler
 */
static irqreturn_t omap_hsmmc_irq(int irq, void *dev_id)
{
	struct omap_hsmmc_host *host = dev_id;
	int status;

	status = OMAP_HSMMC_READ(host->base, STAT);
	do {
		omap_hsmmc_do_irq(host, status);
		/* Flush posted write */
		status = OMAP_HSMMC_READ(host->base, STAT);
	} while (status & INT_EN_MASK);

	return IRQ_HANDLED;
}

static void set_sd_bus_power(struct omap_hsmmc_host *host)
{
	unsigned long i;

	OMAP_HSMMC_WRITE(host->base, HCTL,
			 OMAP_HSMMC_READ(host->base, HCTL) | SDBP);
	for (i = 0; i < loops_per_jiffy; i++) {
		if (OMAP_HSMMC_READ(host->base, HCTL) & SDBP)
			break;
		cpu_relax();
	}
}

/*
 * Switch MMC interface voltage ... only relevant for MMC1.
 *
 * MMC2 and MMC3 use fixed 1.8V levels, and maybe a transceiver.
 * The MMC2 transceiver controls are used instead of DAT4..DAT7.
 * Some chips, like eMMC ones, use internal transceivers.
 */
static int omap_hsmmc_switch_opcond(struct omap_hsmmc_host *host, int vdd)
{
	u32 reg_val = 0;
	int ret;

	/* Disable the clocks */
	pm_runtime_put_sync(host->dev);
	if (host->got_dbclk)
		clk_disable(host->dbclk);

	/* Turn the power off */
	ret = mmc_slot(host).set_power(host->dev, host->slot_id, 0, 0);

	/* Turn the power ON with given VDD 1.8 or 3.0v */
	if (!ret)
		ret = mmc_slot(host).set_power(host->dev, host->slot_id, 1,
					       vdd);
	pm_runtime_get_sync(host->dev);
	if (host->got_dbclk)
		clk_enable(host->dbclk);

	if (ret != 0)
		goto err;

	OMAP_HSMMC_WRITE(host->base, HCTL,
		OMAP_HSMMC_READ(host->base, HCTL) & SDVSCLR);
	reg_val = OMAP_HSMMC_READ(host->base, HCTL);

	/*
	 * If a MMC dual voltage card is detected, the set_ios fn calls
	 * this fn with VDD bit set for 1.8V. Upon card removal from the
	 * slot, omap_hsmmc_set_ios sets the VDD back to 3V on MMC_POWER_OFF.
	 *
	 * Cope with a bit of slop in the range ... per data sheets:
	 *  - "1.8V" for vdds_mmc1/vdds_mmc1a can be up to 2.45V max,
	 *    but recommended values are 1.71V to 1.89V
	 *  - "3.0V" for vdds_mmc1/vdds_mmc1a can be up to 3.5V max,
	 *    but recommended values are 2.7V to 3.3V
	 *
	 * Board setup code shouldn't permit anything very out-of-range.
	 * TWL4030-family VMMC1 and VSIM regulators are fine (avoiding the
	 * middle range) but VSIM can't power DAT4..DAT7 at more than 3V.
	 */
	if ((1 << vdd) <= MMC_VDD_23_24)
		reg_val |= SDVS18;
	else
		reg_val |= SDVS30;

	OMAP_HSMMC_WRITE(host->base, HCTL, reg_val);
	set_sd_bus_power(host);

	return 0;
err:
	dev_dbg(mmc_dev(host->mmc), "Unable to switch operating voltage\n");
	return ret;
}

/* Protect the card while the cover is open */
static void omap_hsmmc_protect_card(struct omap_hsmmc_host *host)
{
	if (!mmc_slot(host).get_cover_state)
		return;

	host->reqs_blocked = 0;
	if (mmc_slot(host).get_cover_state(host->dev, host->slot_id)) {
		if (host->protect_card) {
			printk(KERN_INFO "%s: cover is closed, "
					 "card is now accessible\n",
					 mmc_hostname(host->mmc));
			host->protect_card = 0;
		}
	} else {
		if (!host->protect_card) {
			printk(KERN_INFO "%s: cover is open, "
					 "card is now inaccessible\n",
					 mmc_hostname(host->mmc));
			host->protect_card = 1;
		}
	}
}

/*
 * Work Item to notify the core about card insertion/removal
 */
static void omap_hsmmc_detect(struct work_struct *work)
{
	struct omap_hsmmc_host *host =
		container_of(work, struct omap_hsmmc_host, mmc_carddetect_work);
	struct omap_mmc_slot_data *slot = &mmc_slot(host);
	int carddetect;

	if (host->suspended)
		return;

	sysfs_notify(&host->mmc->class_dev.kobj, NULL, "cover_switch");

	if (slot->card_detect) {
		carddetect = slot->card_detect(host->dev, host->slot_id);
		/* LGE_SJIT 2011-11-28 [dojip.kim@lge.com]
		 * Enhanced the error handling
		 */
		host->eject = !carddetect;
	}
	else {
		omap_hsmmc_protect_card(host);
		carddetect = -ENOSYS;
	}

#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[START]
	if (carddetect)
		mmc_detect_change(host->mmc, (HZ * 2000) / 1000);
	else
	{
	/*
	 * Because of OMAP4 Silicon errata (i705), we have to turn off the
	 * PBIAS and VMMC for SD card as soon as we get card disconnect
	 * interrupt. Because of this, we don't wait for all higher layer
	 * structures to be dismantled before turning off power
	 */
		mmc_claim_host(host->mmc);
		if ((MMC_POWER_OFF != host->power_mode) &&
			(mmc_slot(host).set_power != NULL)){
			mmc_slot(host).set_power(host->dev, host->slot_id, 0, 0);
			host->power_mode = MMC_POWER_OFF;
		}
		mmc_release_host(host->mmc);
		mmc_detect_change(host->mmc, (HZ * 500) / 1000);
	}
#else
	if (carddetect){
		mmc_detect_change(host->mmc, (HZ * 200) / 1000);
	}
	else{
	/*
	 * Because of OMAP4 Silicon errata (i705), we have to turn off the
	 * PBIAS and VMMC for SD card as soon as we get card disconnect
	 * interrupt. Because of this, we don't wait for all higher layer
	 * structures to be dismantled before turning off power
	 */
		mmc_claim_host(host->mmc);
		if ((MMC_POWER_OFF != host->power_mode) &&
			(mmc_slot(host).set_power != NULL)){
			mmc_slot(host).set_power(host->dev, host->slot_id, 0, 0);
			host->power_mode = MMC_POWER_OFF;
		}
		mmc_release_host(host->mmc);
		mmc_detect_change(host->mmc, 0);
	}
#endif
}

/*
 * ISR for handling card insertion and removal
 */
static irqreturn_t omap_hsmmc_cd_handler(int irq, void *dev_id)
{
	struct omap_hsmmc_host *host = (struct omap_hsmmc_host *)dev_id;

	if (host->suspended)
		return IRQ_HANDLED;
	schedule_work(&host->mmc_carddetect_work);

	return IRQ_HANDLED;
}

static int omap_hsmmc_get_dma_sync_dev(struct omap_hsmmc_host *host,
				     struct mmc_data *data)
{
	int sync_dev;

	if (data->flags & MMC_DATA_WRITE)
		sync_dev = host->dma_line_tx;
	else
		sync_dev = host->dma_line_rx;
	return sync_dev;
}

static void omap_hsmmc_config_dma_params(struct omap_hsmmc_host *host,
				       struct mmc_data *data,
				       struct scatterlist *sgl)
{
	int blksz, nblk, dma_ch;

	dma_ch = host->dma_ch;
	if (data->flags & MMC_DATA_WRITE) {
		omap_set_dma_dest_params(dma_ch, 0, OMAP_DMA_AMODE_CONSTANT,
			(host->mapbase + OMAP_HSMMC_DATA), 0, 0);
		omap_set_dma_src_params(dma_ch, 0, OMAP_DMA_AMODE_POST_INC,
			sg_dma_address(sgl), 0, 0);
	} else {
		omap_set_dma_src_params(dma_ch, 0, OMAP_DMA_AMODE_CONSTANT,
			(host->mapbase + OMAP_HSMMC_DATA), 0, 0);
		omap_set_dma_dest_params(dma_ch, 0, OMAP_DMA_AMODE_POST_INC,
			sg_dma_address(sgl), 0, 0);
	}

	blksz = host->data->blksz;
	nblk = sg_dma_len(sgl) / blksz;

	omap_set_dma_transfer_params(dma_ch, OMAP_DMA_DATA_TYPE_S32,
			blksz / 4, nblk, OMAP_DMA_SYNC_FRAME,
			omap_hsmmc_get_dma_sync_dev(host, data),
			!(data->flags & MMC_DATA_WRITE));

	omap_start_dma(dma_ch);
}

/*
 * DMA call back function
 */
static void omap_hsmmc_dma_cb(int lch, u16 ch_status, void *cb_data)
{
	struct omap_hsmmc_host *host = cb_data;
	struct mmc_data *data = host->mrq->data;
	int dma_ch, req_in_progress;

	if (!(ch_status & OMAP_DMA_BLOCK_IRQ)) {
		dev_warn(mmc_dev(host->mmc), "unexpected dma status %x\n",
			ch_status);
		return;
	}

	spin_lock(&host->irq_lock);
	if (host->dma_ch < 0) {
		spin_unlock(&host->irq_lock);
		return;
	}

	host->dma_sg_idx++;
	if (host->dma_sg_idx < host->dma_len) {
		/* Fire up the next transfer. */
		omap_hsmmc_config_dma_params(host, data,
					   data->sg + host->dma_sg_idx);
		spin_unlock(&host->irq_lock);
		return;
	}

	dma_unmap_sg(mmc_dev(host->mmc), data->sg, data->sg_len,
		omap_hsmmc_get_dma_dir(host, data));

	req_in_progress = host->req_in_progress;
	dma_ch = host->dma_ch;
	host->dma_ch = -1;
	spin_unlock(&host->irq_lock);

	omap_free_dma(dma_ch);

	/* If DMA has finished after TC, complete the request */
	if (!req_in_progress) {
		struct mmc_request *mrq = host->mrq;

		host->mrq = NULL;
		mmc_request_done(host->mmc, mrq);
	}
}

/*
 * Routine to configure and start DMA for the MMC card
 */
static int omap_hsmmc_start_sdma_transfer(struct omap_hsmmc_host *host,
					struct mmc_request *req)
{
	int dma_ch = 0, ret = 0, i;
	struct mmc_data *data = req->data;

	/* Sanity check: all the SG entries must be aligned by block size. */
	for (i = 0; i < data->sg_len; i++) {
		struct scatterlist *sgl;

		sgl = data->sg + i;
		if (sgl->length % data->blksz)
			return -EINVAL;
	}
	if ((data->blksz % 4) != 0)
		/* REVISIT: The MMC buffer increments only when MSB is written.
		 * Return error for blksz which is non multiple of four.
		 */
		return -EINVAL;

	BUG_ON(host->dma_ch != -1);

	ret = omap_request_dma(omap_hsmmc_get_dma_sync_dev(host, data),
			       "MMC/SD", omap_hsmmc_dma_cb, host, &dma_ch);
	if (ret != 0) {
		dev_err(mmc_dev(host->mmc),
			"%s: omap_request_dma() failed with %d\n",
			mmc_hostname(host->mmc), ret);
		return ret;
	}

	host->dma_len = dma_map_sg(mmc_dev(host->mmc), data->sg,
			data->sg_len, omap_hsmmc_get_dma_dir(host, data));
	host->dma_ch = dma_ch;
	host->dma_sg_idx = 0;

	omap_hsmmc_config_dma_params(host, data, data->sg);

	return 0;
}

static int mmc_populate_adma_desc_table(struct omap_hsmmc_host *host,
		struct mmc_request *req, struct adma_desc_table *pdesc)
{
	int i, j, dmalen;
	int splitseg, xferaddr;
	int numblocks = 0;
	dma_addr_t dmaaddr;
	struct mmc_data *data = req->data;

	host->dma_len = dma_map_sg(mmc_dev(host->mmc), data->sg,
			data->sg_len, omap_hsmmc_get_dma_dir(host, data));
	for (i = 0, j = 0; i < host->dma_len; i++) {
		dmaaddr = sg_dma_address(data->sg + i);
		dmalen = sg_dma_len(data->sg + i);
		numblocks += dmalen / data->blksz;

		if (dmalen <= ADMA_MAX_XFER_PER_ROW) {

			pdesc[i + j].length = dmalen;
			pdesc[i + j].addr = dmaaddr;
			pdesc[i + j].attr = (ADMA_XFER_DESC |
				ADMA_XFER_VALID);

		} else {
			/* Each descritpor row can only support
			 * transfer upto ADMA_MAX_XFER_PER_ROW.
			 * If the current segment is bigger, it has to be
			 * split to multiple ADMA table entries.
			 */
			xferaddr = 0;
			do {
				splitseg = min(dmalen, ADMA_MAX_XFER_PER_ROW);
				dmalen -= splitseg;
				pdesc[i + j].length = splitseg;
				pdesc[i + j].addr =
					dmaaddr + xferaddr;
				xferaddr += splitseg;
				pdesc[i + j].attr = (ADMA_XFER_DESC |
					ADMA_XFER_VALID);
				j++;
			} while (dmalen);
			j--; /* Compensate for i++ */
		}
	}
	/* Setup last entry to terminate */
	pdesc[i + j - 1].attr |= ADMA_XFER_END;
	WARN_ON((i + j - 1) > DMA_TABLE_NUM_ENTRIES);
	dev_dbg(mmc_dev(host->mmc),
		"ADMA table has %d entries from %d sglist\n",
		i + j, host->dma_len);
	return numblocks;
}

static void omap_hsmmc_start_adma_transfer(struct omap_hsmmc_host *host)
{
	wmb();
	OMAP_HSMMC_WRITE(host->base, ADMA_SAL, host->phy_adma_table);
}

static void set_data_timeout(struct omap_hsmmc_host *host,
			     unsigned int timeout_ns,
			     unsigned int timeout_clks)
{
	unsigned int timeout, cycle_ns;
	uint32_t reg, clkd, dto = 0;

	reg = OMAP_HSMMC_READ(host->base, SYSCTL);
	clkd = (reg & CLKD_MASK) >> CLKD_SHIFT;
	if (clkd == 0)
		clkd = 1;

	cycle_ns = 1000000000 / (clk_get_rate(host->fclk) / clkd);
	timeout = timeout_ns / cycle_ns;
	timeout += timeout_clks;
	if (timeout) {
		while ((timeout & 0x80000000) == 0) {
			dto += 1;
			timeout <<= 1;
		}
		dto = 31 - dto;
		timeout <<= 1;
		if (timeout && dto)
			dto += 1;
		if (dto >= 13)
			dto -= 13;
		else
			dto = 0;
		if (dto > 14)
			dto = 14;
	}

//mo2haewoon.you@lge.com => [START]
#ifdef CONFIG_MACH_LGE
	dto = 14; 
#endif
//mo2haewoon.you@lge.com <= [END] 

	reg &= ~DTO_MASK;
	reg |= dto << DTO_SHIFT;
	OMAP_HSMMC_WRITE(host->base, SYSCTL, reg);
}

/*
 * Configure block length for MMC/SD cards and initiate the transfer.
 */
static int
omap_hsmmc_prepare_data(struct omap_hsmmc_host *host, struct mmc_request *req)
{
	int ret;
	int numblks;

	host->data = req->data;

	if (req->data == NULL) {
		OMAP_HSMMC_WRITE(host->base, BLK, 0);
		/*
		 * Set an arbitrary 100ms data timeout for commands with
		 * busy signal.
		 */
		if (req->cmd->flags & MMC_RSP_BUSY)
			set_data_timeout(host, 100000000U, 0);
		return 0;
	}

	OMAP_HSMMC_WRITE(host->base, BLK, (req->data->blksz)
					| (req->data->blocks << 16));
	set_data_timeout(host, req->data->timeout_ns, req->data->timeout_clks);

	if (host->dma_type == SDMA_XFER) {
		ret = omap_hsmmc_start_sdma_transfer(host, req);
		if (ret != 0) {
			dev_dbg(mmc_dev(host->mmc), "MMC start dma failure\n");
			return ret;
		}
	} else if (host->dma_type == ADMA_XFER) {
		numblks = mmc_populate_adma_desc_table(host,
				req, host->adma_table);
		WARN_ON(numblks != req->data->blocks);
		omap_hsmmc_start_adma_transfer(host);
	}
	return 0;
}

/*
 * Request function. for read/write operation
 */
static void omap_hsmmc_request(struct mmc_host *mmc, struct mmc_request *req)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);
	int err;

	BUG_ON(host->req_in_progress);
	BUG_ON(host->dma_ch != -1);
	/* LGE_SJIT_S 2011-11-28 [dojip.kim@lge.com]
	 * Enhanced the error handling
	 */
	if (host->eject) {
		/*
		 * Ensure the controller is left in a consistent
		 * state by resetting the command and data state
		 * machines.
		 */
		omap_hsmmc_reset_controller_fsm(host, SRD);
		omap_hsmmc_reset_controller_fsm(host, SRC);

		if (req->data && !(req->data->flags & MMC_DATA_READ)) {
			req->cmd->error = 0;
			req->data->bytes_xfered = req->data->blksz *
						  req->data->blocks;
		} else
			req->cmd->error = -ENOMEDIUM;

		req->cmd->retries = 0;
		mmc_request_done(mmc, req);
		return;
	}
	/* LGE_SJIT_E 2011-11-28 [dojip.kim@lge.com] */
	if (host->protect_card) {
		if (host->reqs_blocked < 3) {
			/*
			 * Ensure the controller is left in a consistent
			 * state by resetting the command and data state
			 * machines.
			 */
			omap_hsmmc_reset_controller_fsm(host, SRD);
			omap_hsmmc_reset_controller_fsm(host, SRC);
			host->reqs_blocked += 1;
		}
		req->cmd->error = -EBADF;
		if (req->data)
			req->data->error = -EBADF;
		req->cmd->retries = 0;
		mmc_request_done(mmc, req);
		return;
	} else if (host->reqs_blocked)
		host->reqs_blocked = 0;
	WARN_ON(host->mrq != NULL);

	/*
	 * Because of OMAP4 Silicon errata (i705), we have to turn off the
	 * PBIAS and VMMC for SD card as soon as we get card disconnect
	 * interrupt. Because of this, we don't wait for all higher layer
	 * structures to be dismantled before turning off power. Because
	 * of this, we might end up here even after SD card is removed
	 * and VMMC and PBIAS are turned off. In that case, just fail
	 * the commands immediately
	 */
	if (host->power_mode == MMC_POWER_OFF) {
		req->cmd->error = EIO;
		if (req->data)
			req->data->error = -EIO;
		dev_warn(mmc_dev(host->mmc),
		"Card is no longer present\n");
		mmc_request_done(mmc, req);
		return;
	}

	host->mrq = req;
	if (req->sbc) {
		omap_hsmmc_start_command(host, req->sbc, NULL, 0);
		return;
	}

	err = omap_hsmmc_prepare_data(host, req);
	if (err) {
		req->cmd->error = err;
		if (req->data)
			req->data->error = err;
		host->mrq = NULL;
		mmc_request_done(mmc, req);
		return;
	}

	omap_hsmmc_start_command(host, req->cmd, req->data, 0);
}

/* Routine to configure clock values. Exposed API to core */
static void omap_hsmmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);
	u16 dsor = 0;
	unsigned long regval;
	unsigned long timeout;
	u32 con;
	int do_send_init_stream = 0;

	mmc_claim_host(host->mmc);

	if (ios->power_mode != host->power_mode) {
		switch (ios->power_mode) {
		case MMC_POWER_OFF:
			mmc_slot(host).set_power(host->dev, host->slot_id,
						 0, 0);
			host->vdd = 0;
			break;
		case MMC_POWER_UP:
			mmc_slot(host).set_power(host->dev, host->slot_id,
						 1, ios->vdd);
			host->vdd = ios->vdd;
			break;
		case MMC_POWER_ON:
			do_send_init_stream = 1;
			break;
		}
		host->power_mode = ios->power_mode;
	}

	/* FIXME: set registers based only on changes to ios */

	con = OMAP_HSMMC_READ(host->base, CON);
	/* configure in DDR mode */
	if (ios->timing == MMC_TIMING_UHS_DDR50) {
		con |= DDR;
	} else {
		con &= ~DDR;
	}
	switch (mmc->ios.bus_width) {
	case MMC_BUS_WIDTH_8:
		OMAP_HSMMC_WRITE(host->base, CON, con | DW8);
		break;
	case MMC_BUS_WIDTH_4:
		OMAP_HSMMC_WRITE(host->base, CON, con & ~DW8);
		OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | FOUR_BIT);
		break;
	case MMC_BUS_WIDTH_1:
		OMAP_HSMMC_WRITE(host->base, CON, con & ~DW8);
		OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) & ~FOUR_BIT);
		break;
	}

	if (host->pdata->controller_flags & OMAP_HSMMC_SUPPORTS_DUAL_VOLT) {
		/* Only MMC1 can interface at 3V without some flavor
		 * of external transceiver; but they all handle 1.8V.
		 */
		if ((OMAP_HSMMC_READ(host->base, HCTL) & SDVSDET) &&
			(ios->vdd == DUAL_VOLT_OCR_BIT)) {
				/*
				 * The mmc_select_voltage fn of the core does
				 * not seem to set the power_mode to
				 * MMC_POWER_UP upon recalculating the voltage.
				 * vdd 1.8v.
				 */
			if (omap_hsmmc_switch_opcond(host, ios->vdd) != 0)
				dev_dbg(mmc_dev(host->mmc),
						"Switch operation failed\n");
		}
	}

	if (ios->clock) {
		dsor = host->master_clock / ios->clock;
		if (dsor < 1)
			dsor = 1;

		if (host->master_clock / dsor > ios->clock)
			dsor++;

		if (dsor > 250)
			dsor = 250;
	}
	omap_hsmmc_stop_clock(host);
	regval = OMAP_HSMMC_READ(host->base, SYSCTL);
	regval = regval & ~(CLKD_MASK);
	regval = regval | (dsor << 6) | (DTO << 16);
	OMAP_HSMMC_WRITE(host->base, SYSCTL, regval);
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) | ICE);

	/* Wait till the ICS bit is set */
	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & ICS) != ICS
		&& time_before(jiffies, timeout))
		msleep(1);

	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) | CEN);

	if (do_send_init_stream)
		send_init_stream(host);

	con = OMAP_HSMMC_READ(host->base, CON);
	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		OMAP_HSMMC_WRITE(host->base, CON, con | OD);
	else
		OMAP_HSMMC_WRITE(host->base, CON, con & ~OD);

	mmc_release_host(host->mmc);
}

static int omap_hsmmc_get_cd(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (!mmc_slot(host).card_detect)
		return -ENOSYS;
	return mmc_slot(host).card_detect(host->dev, host->slot_id);
}

static int omap_hsmmc_get_ro(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (!mmc_slot(host).get_ro)
		return -ENOSYS;
	return mmc_slot(host).get_ro(host->dev, 0);
}

static void omap_hsmmc_init_card(struct mmc_host *mmc, struct mmc_card *card)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (mmc_slot(host).init_card)
		mmc_slot(host).init_card(card);
}

static void omap_hsmmc_conf_bus_power(struct omap_hsmmc_host *host)
{
	u32 hctl, capa, value;

	/* Only MMC1 supports 3.0V */
	if (host->pdata->controller_flags & OMAP_HSMMC_SUPPORTS_DUAL_VOLT) {
		hctl = SDVS30;
		capa = VS30 | VS18;
	} else {
		hctl = SDVS18;
		capa = VS18;
	}

	if (host->dma_type == ADMA_XFER)
		hctl |= DMAS;

	value = OMAP_HSMMC_READ(host->base, HCTL) & ~SDVS_MASK;
	OMAP_HSMMC_WRITE(host->base, HCTL, value | hctl);

	if (host->dma_type == ADMA_XFER) {
		value = OMAP_HSMMC_READ(host->base, CON);
		OMAP_HSMMC_WRITE(host->base, CON, value | DMA_MNS_ADMA_MODE);
	}

	value = OMAP_HSMMC_READ(host->base, CAPA);
	OMAP_HSMMC_WRITE(host->base, CAPA, value | capa);

	/* Set the controller to AUTO IDLE mode */
	value = OMAP_HSMMC_READ(host->base, SYSCONFIG);
	OMAP_HSMMC_WRITE(host->base, SYSCONFIG, value | AUTOIDLE);

	/* Set SD bus power bit */
	set_sd_bus_power(host);
}

/*
 * Dynamic power saving handling, FSM:
 *   ENABLED -> DISABLED -> CARDSLEEP / REGSLEEP -> OFF
 *     ^___________|          |                      |
 *     |______________________|______________________|
 *
 * ENABLED:   mmc host is fully functional
 * DISABLED:  fclk is off
 * CARDSLEEP: fclk is off, card is asleep, voltage regulator is asleep
 * REGSLEEP:  fclk is off, voltage regulator is asleep
 * OFF:       fclk is off, voltage regulator is off
 *
 * Transition handlers return the timeout for the next state transition
 * or negative error.
 */

enum {ENABLED = 0, DISABLED, CARDSLEEP, REGSLEEP, OFF};

/* Handler for [ENABLED -> DISABLED] transition */
static int omap_hsmmc_enabled_to_disabled(struct omap_hsmmc_host *host)
{
	pm_runtime_put_sync(host->dev);

	host->dpm_state = DISABLED;

	dev_dbg(mmc_dev(host->mmc), "ENABLED -> DISABLED\n");

	if (host->power_mode == MMC_POWER_OFF)
		return 0;

	return OMAP_MMC_SLEEP_TIMEOUT;
}

/* Handler for [DISABLED -> REGSLEEP / CARDSLEEP] transition */
static int omap_hsmmc_disabled_to_sleep(struct omap_hsmmc_host *host)
{
	int err, new_state;

	if (!mmc_try_claim_host(host->mmc))
		return 0;

	pm_runtime_get_sync(host->dev);

	if (mmc_card_can_sleep(host->mmc)) {
		err = mmc_card_sleep(host->mmc);
		if (err < 0) {
			pm_runtime_put_sync(host->dev);
			mmc_release_host(host->mmc);
			return err;
		}
		new_state = CARDSLEEP;
	} else {
		new_state = REGSLEEP;
	}
	if (mmc_slot(host).set_sleep)
		mmc_slot(host).set_sleep(host->dev, host->slot_id, 1, 0,
					 new_state == CARDSLEEP);
	/* FIXME: turn off bus power and perhaps interrupts too */
	pm_runtime_put_sync(host->dev);
	host->dpm_state = new_state;

	mmc_release_host(host->mmc);

	dev_dbg(mmc_dev(host->mmc), "DISABLED -> %s\n",
		host->dpm_state == CARDSLEEP ? "CARDSLEEP" : "REGSLEEP");

	if (mmc_slot(host).no_off)
		return 0;

	if ((host->mmc->caps & MMC_CAP_NONREMOVABLE) ||
	    mmc_slot(host).card_detect ||
	    (mmc_slot(host).get_cover_state &&
	     mmc_slot(host).get_cover_state(host->dev, host->slot_id)))
		return OMAP_MMC_OFF_TIMEOUT;

	return 0;
}

/* Handler for [REGSLEEP / CARDSLEEP -> OFF] transition */
static int omap_hsmmc_sleep_to_off(struct omap_hsmmc_host *host)
{
	if (!mmc_try_claim_host(host->mmc))
		return 0;

	if (mmc_slot(host).no_off)
		return 0;

	if (!((host->mmc->caps & MMC_CAP_NONREMOVABLE) ||
	      mmc_slot(host).card_detect ||
	      (mmc_slot(host).get_cover_state &&
	       mmc_slot(host).get_cover_state(host->dev, host->slot_id)))) {
		mmc_release_host(host->mmc);
		return 0;
	}

	mmc_slot(host).set_power(host->dev, host->slot_id, 0, 0);
	host->vdd = 0;
	host->power_mode = MMC_POWER_OFF;

	dev_dbg(mmc_dev(host->mmc), "%s -> OFF\n",
		host->dpm_state == CARDSLEEP ? "CARDSLEEP" : "REGSLEEP");

	host->dpm_state = OFF;

	mmc_release_host(host->mmc);

	return 0;
}

#ifdef CONFIG_OMAP4_DPLL_CASCADING
static void
omap_hsmmc_dpll_clocks_configure(struct omap_hsmmc_host *host, int clk)
{
	int regval = 0, dsor = 0;
	unsigned long timeout = 0;
	u32 count = 0;

	if (host->mmc->ios.clock != 0)
		dsor = clk / host->mmc->ios.clock;
	omap_hsmmc_stop_clock(host);
	regval = OMAP_HSMMC_READ(host->base, SYSCTL);
	regval = regval & ~(CLKD_MASK);
	regval = regval | (dsor << 6) | (DTO << 16);
	OMAP_HSMMC_WRITE(host->base, SYSCTL, regval);
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
	OMAP_HSMMC_READ(host->base, SYSCTL) | ICE);
	/* Wait till the ICS bit is set */
	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & ICS) != ICS
		&& time_before(jiffies, timeout)) {
		if (count++ > 1000) {
			dev_err(host->dev,
				"Timeout during waiting for ICS bit");
			break;
		}
	}

	OMAP_HSMMC_WRITE(host->base, SYSCTL,
	OMAP_HSMMC_READ(host->base, SYSCTL) | CEN);
}

static void
omap_hsmmc_dpll_clocks_reconfigure(struct omap_hsmmc_host *host)
{
	spin_lock(&host->dpll_lock);
	if (host->dpll_entry == 1) {
		omap_hsmmc_dpll_clocks_configure(host, OMAP_MMC_DPLL_CLOCK);
		host->dpll_entry = 0;
	} else if (host->dpll_exit == 1) {
		omap_hsmmc_dpll_clocks_configure(host, OMAP_MMC_MASTER_CLOCK);
		host->dpll_exit = 0;
	}
	spin_unlock(&host->dpll_lock);
}
#endif

/* Handler for [DISABLED -> ENABLED] transition */
static int omap_hsmmc_disabled_to_enabled(struct omap_hsmmc_host *host)
{
	pm_runtime_get_sync(host->dev);

#ifdef CONFIG_OMAP4_DPLL_CASCADING
	omap_hsmmc_dpll_clocks_reconfigure(host);
#endif
	host->dpm_state = ENABLED;

	dev_dbg(mmc_dev(host->mmc), "DISABLED -> ENABLED\n");

	return 0;
}

/* Handler for [SLEEP -> ENABLED] transition */
static int omap_hsmmc_sleep_to_enabled(struct omap_hsmmc_host *host)
{
	if (!mmc_try_claim_host(host->mmc))
		return 0;

	pm_runtime_get_sync(host->dev);

#ifdef CONFIG_OMAP4_DPLL_CASCADING
	omap_hsmmc_dpll_clocks_reconfigure(host);
#endif
	if (mmc_slot(host).set_sleep)
		mmc_slot(host).set_sleep(host->dev, host->slot_id, 0,
			 host->vdd, host->dpm_state == CARDSLEEP);
	if (mmc_card_can_sleep(host->mmc))
		mmc_card_awake(host->mmc);

	dev_dbg(mmc_dev(host->mmc), "%s -> ENABLED\n",
		host->dpm_state == CARDSLEEP ? "CARDSLEEP" : "REGSLEEP");

	host->dpm_state = ENABLED;

	mmc_release_host(host->mmc);

	return 0;
}

/* Handler for [OFF -> ENABLED] transition */
static int omap_hsmmc_off_to_enabled(struct omap_hsmmc_host *host)
{
	pm_runtime_get_sync(host->dev);

	omap_hsmmc_conf_bus_power(host);
	mmc_power_restore_host(host->mmc);

	host->dpm_state = ENABLED;

	dev_dbg(mmc_dev(host->mmc), "OFF -> ENABLED\n");

	return 0;
}

/*
 * Bring MMC host to ENABLED from any other PM state.
 */
static int omap_hsmmc_enable(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	switch (host->dpm_state) {
	case DISABLED:
		return omap_hsmmc_disabled_to_enabled(host);
	case CARDSLEEP:
	case REGSLEEP:
		return omap_hsmmc_sleep_to_enabled(host);
	case OFF:
		return omap_hsmmc_off_to_enabled(host);
	default:
		dev_dbg(mmc_dev(host->mmc), "UNKNOWN state\n");
		return -EINVAL;
	}
}

/*
 * Bring MMC host in PM state (one level deeper).
 */
static int omap_hsmmc_disable(struct mmc_host *mmc, int lazy)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	switch (host->dpm_state) {
	case ENABLED: {
		int delay;

		delay = omap_hsmmc_enabled_to_disabled(host);
		if (lazy || delay < 0)
			return delay;
		return 0;
	}
	case DISABLED:
		return omap_hsmmc_disabled_to_sleep(host);
	case CARDSLEEP:
	case REGSLEEP:
		return omap_hsmmc_sleep_to_off(host);
	default:
		dev_dbg(mmc_dev(host->mmc), "UNKNOWN state\n");
		return -EINVAL;
	}
}

static int omap_hsmmc_enable_simple(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	pm_runtime_get_sync(host->dev);

	dev_dbg(mmc_dev(host->mmc), "enabled\n");
	return 0;
}

static int omap_hsmmc_disable_simple(struct mmc_host *mmc, int lazy)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	pm_runtime_put_sync(host->dev);

	dev_dbg(mmc_dev(host->mmc), "idle\n");
	return 0;
}

static const struct mmc_host_ops omap_hsmmc_ops = {
	.enable = omap_hsmmc_enable_simple,
	.disable = omap_hsmmc_disable_simple,
	.request = omap_hsmmc_request,
	.set_ios = omap_hsmmc_set_ios,
	.get_cd = omap_hsmmc_get_cd,
	.get_ro = omap_hsmmc_get_ro,
	.init_card = omap_hsmmc_init_card,
	/* NYET -- enable_sdio_irq */
};

static const struct mmc_host_ops omap_hsmmc_ps_ops = {
	.enable = omap_hsmmc_enable,
	.disable = omap_hsmmc_disable,
	.request = omap_hsmmc_request,
	.set_ios = omap_hsmmc_set_ios,
	.get_cd = omap_hsmmc_get_cd,
	.get_ro = omap_hsmmc_get_ro,
	.init_card = omap_hsmmc_init_card,
	/* NYET -- enable_sdio_irq */
};

#ifdef CONFIG_DEBUG_FS

static int omap_hsmmc_regs_show(struct seq_file *s, void *data)
{
	struct mmc_host *mmc = s->private;
	struct omap_hsmmc_host *host = mmc_priv(mmc);


	seq_printf(s, "mmc%d:\n"
			" enabled:\t%d\n"
			" dpm_state:\t%d\n"
			" nesting_cnt:\t%d\n"
			" ct",
			mmc->index, mmc->enabled ? 1 : 0,
			host->dpm_state, mmc->nesting_cnt);

	if (host->suspended || host->dpm_state == OFF) {
		seq_printf(s, "host suspended, can't read registers\n");
		return 0;
	}

	pm_runtime_get_sync(host->dev);

	seq_printf(s, "SYSCONFIG:\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, SYSCONFIG));
	seq_printf(s, "CON:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, CON));
	seq_printf(s, "HCTL:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, HCTL));
	seq_printf(s, "SYSCTL:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, SYSCTL));
	seq_printf(s, "IE:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, IE));
	seq_printf(s, "ISE:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, ISE));
	seq_printf(s, "CAPA:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, CAPA));

	pm_runtime_put_sync(host->dev);

	return 0;
}

static int omap_hsmmc_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, omap_hsmmc_regs_show, inode->i_private);
}

static const struct file_operations mmc_regs_fops = {
	.open           = omap_hsmmc_regs_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static void omap_hsmmc_debugfs(struct mmc_host *mmc)
{
	if (mmc->debugfs_root)
		debugfs_create_file("regs", S_IRUSR, mmc->debugfs_root,
			mmc, &mmc_regs_fops);
}

#else

static void omap_hsmmc_debugfs(struct mmc_host *mmc)
{
}

#endif

#ifdef CONFIG_OMAP4_DPLL_CASCADING
static int omap_hsmmc_dpll_notifier(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct omap_hsmmc_host *host =
		container_of(nb, struct omap_hsmmc_host, nb);
	struct clk_notifier_data *cnd = (struct clk_notifier_data *)data;

	spin_lock(&host->dpll_lock);

	if (val != CLK_PRE_RATE_CHANGE) {
		if (cnd->old_rate == OMAP_MMC_DPLL_CLOCK) {
			host->dpll_entry = 1;
			host->master_clock = OMAP_MMC_DPLL_CLOCK;
		} else if (cnd->old_rate == OMAP_MMC_MASTER_CLOCK) {
			host->dpll_exit = 1;
			host->master_clock = OMAP_MMC_MASTER_CLOCK;
		}
	}

	spin_unlock(&host->dpll_lock);
	return 0;
}
#endif

static int __init omap_hsmmc_probe(struct platform_device *pdev)
{
	struct omap_mmc_platform_data *pdata = pdev->dev.platform_data;
	struct mmc_host *mmc;
	struct omap_hsmmc_host *host = NULL;
	struct resource *res;
	int ret, irq;
	int ctrlr_caps = 0;

	if (pdata == NULL) {
		dev_err(&pdev->dev, "Platform Data is missing\n");
		return -ENXIO;
	}

	if (pdata->nr_slots == 0) {
		dev_err(&pdev->dev, "No Slots\n");
		return -ENXIO;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (res == NULL || irq < 0)
		return -ENXIO;

	res->start += pdata->reg_offset;
	res->end += pdata->reg_offset;
	res = request_mem_region(res->start, resource_size(res), pdev->name);
	if (res == NULL)
		return -EBUSY;

	ret = omap_hsmmc_gpio_init(pdata);
	if (ret)
		goto err;

	mmc = mmc_alloc_host(sizeof(struct omap_hsmmc_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	host		= mmc_priv(mmc);
	host->mmc	= mmc;
	host->pdata	= pdata;
	host->dev	= &pdev->dev;
	host->dma_type	= SDMA_XFER;
	host->dev->dma_mask = &pdata->dma_mask;
	host->dma_ch	= -1;
	host->irq	= irq;
	host->id	= pdev->id;
	host->slot_id	= 0;
	host->mapbase	= res->start;
	host->base	= ioremap(host->mapbase, SZ_4K);
	if (!host->base) {
		ret = -ENOMEM;
		goto err_ioremap;
	}
	host->power_mode = MMC_POWER_OFF;
	host->flags	= AUTO_CMD12;

	host->master_clock = OMAP_MMC_MASTER_CLOCK;
	if (mmc_slot(host).features & HSMMC_HAS_48MHZ_MASTER_CLK)
		host->master_clock = OMAP_MMC_MASTER_CLOCK / 2;

	platform_set_drvdata(pdev, host);
	INIT_WORK(&host->mmc_carddetect_work, omap_hsmmc_detect);

	if (mmc_slot(host).power_saving)
		mmc->ops	= &omap_hsmmc_ps_ops;
	else
		mmc->ops	= &omap_hsmmc_ops;

	/*
	 * If regulator_disable can only put vcc_aux to sleep then there is
	 * no off state.
	 */
	if (mmc_slot(host).vcc_aux_disable_is_sleep)
		mmc_slot(host).no_off = 1;

	mmc->f_min	= 400000;
	mmc->f_max	= 52000000;

	spin_lock_init(&host->irq_lock);

	host->iclk = clk_get(&pdev->dev, "ick");
	if (IS_ERR(host->iclk)) {
		ret = PTR_ERR(host->iclk);
		host->iclk = NULL;
		goto err1;
	}
	host->fclk = clk_get(&pdev->dev, "fck");
	if (IS_ERR(host->fclk)) {
		ret = PTR_ERR(host->fclk);
		host->fclk = NULL;
		clk_put(host->iclk);
		goto err1;
	}

#ifdef CONFIG_OMAP4_DPLL_CASCADING
	if (0 == host->id) {
		spin_lock_init(&host->dpll_lock);
		host->nb.notifier_call = omap_hsmmc_dpll_notifier;
		host->nb.next = NULL;
		clk_notifier_register(host->fclk, &host->nb);
	}
#endif

	omap_hsmmc_context_save(host);

	mmc->caps |= MMC_CAP_DISABLE;
	mmc_set_disable_delay(mmc, OMAP_MMC_DISABLED_TIMEOUT);
	/* we start off in DISABLED state */
	host->dpm_state = DISABLED;

	pm_runtime_enable(host->dev);
#ifndef CONFIG_PM_RUNTIME
	/*
	 * If runtime PM is not enabled, ensure clocks are always enabled.
	 */
	clk_enable(host->iclk);
	clk_enable(host->fclk);
#endif

	if (mmc_host_enable(host->mmc) != 0)
		goto err1;

	if (cpu_is_omap2430()) {
		host->dbclk = clk_get(&pdev->dev, "mmchsdb_fck");
		/*
		 * MMC can still work without debounce clock.
		 */
		if (IS_ERR(host->dbclk))
			dev_warn(mmc_dev(host->mmc),
				"Failed to get debounce clock\n");
		else
			host->got_dbclk = 1;

		if (host->got_dbclk)
			if (clk_enable(host->dbclk) != 0)
				dev_dbg(mmc_dev(host->mmc), "Enabling debounce"
							" clk failed\n");
	}

	ctrlr_caps = OMAP_HSMMC_READ(host->base, CAPA);
	if (ctrlr_caps & CAPA_ADMA_SUPPORT) {
		/* FIXME: passing the device structure fails
		 * due to unset conherency mask
		 */
		host->adma_table = dma_alloc_coherent(NULL,
			ADMA_TABLE_SZ, &host->phy_adma_table, 0);
		if (host->adma_table != NULL)
			host->dma_type = ADMA_XFER;
	}
	dev_dbg(mmc_dev(host->mmc), "DMA Mode=%d\n", host->dma_type);

	/* Since we do only SG emulation, we can have as many segs
	 * as we want. */

	if (host->dma_type == ADMA_XFER) {
		/* Worst case is when above block layer gives us 512 segments,
		  * in which there are 511 single block entries, but one large
		  * block that is of size mmc->max_req_size - (511*512) bytes.
		  * In this case, we use the reserved 512 table entries to
		  * break up the large request. This is also the reason why we
		  * say we can only handle DMA_TABLE_NUM_ENTRIES/2
		  * segments instead of DMA_TABLE_NUM_ENTRIES.
		  */
		mmc->max_segs = DMA_TABLE_NUM_ENTRIES/2;
		mmc->max_blk_size = 512;       /* Block Length at max can be 1024 */
		mmc->max_blk_count = ADMA_MAX_BLKS_PER_ROW *
							DMA_TABLE_NUM_ENTRIES/2;
		mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;
	} else {
		mmc->max_segs = DMA_TABLE_NUM_ENTRIES;
		mmc->max_blk_size = 512;       /* Block Length at max can be 1024 */
		mmc->max_blk_count = 0xFFFF;    /* No. of Blocks is 16 bits */
		mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;
	}
	mmc->max_seg_size = mmc->max_req_size;

	mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED |
		     MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_ERASE | MMC_CAP_CMD23;

	mmc->caps |= mmc_slot(host).caps;
	if (mmc->caps & MMC_CAP_8_BIT_DATA)
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	if (mmc_slot(host).nonremovable)
		mmc->caps |= MMC_CAP_NONREMOVABLE;

	mmc->pm_caps = MMC_PM_KEEP_POWER | MMC_PM_IGNORE_PM_NOTIFY;
	if (mmc_slot(host).mmc_data.built_in)
		mmc->pm_flags = MMC_PM_KEEP_POWER | MMC_PM_IGNORE_PM_NOTIFY;

	omap_hsmmc_conf_bus_power(host);

	res = platform_get_resource_byname(pdev, IORESOURCE_DMA, "tx");
	if (!res) {
		dev_err(mmc_dev(host->mmc), "cannot get DMA TX channel\n");
		goto err_irq;
	}
	host->dma_line_tx = res->start;

	res = platform_get_resource_byname(pdev, IORESOURCE_DMA, "rx");
	if (!res) {
		dev_err(mmc_dev(host->mmc), "cannot get DMA RX channel\n");
		goto err_irq;
	}
	host->dma_line_rx = res->end;

	/* Request IRQ for MMC operations */
	ret = request_irq(host->irq, omap_hsmmc_irq, IRQF_DISABLED,
			mmc_hostname(mmc), host);
	if (ret) {
		dev_dbg(mmc_dev(host->mmc), "Unable to grab HSMMC IRQ\n");
		goto err_irq;
	}

	if (pdata->init != NULL) {
		if (pdata->init(&pdev->dev) != 0) {
			dev_dbg(mmc_dev(host->mmc),
				"Unable to configure MMC IRQs\n");
			goto err_irq_cd_init;
		}
	}

	if (omap_hsmmc_have_reg() && !mmc_slot(host).set_power) {
		ret = omap_hsmmc_reg_get(host);
		if (ret)
			goto err_reg;
		host->use_reg = 1;
	}

	mmc->ocr_avail = mmc_slot(host).ocr_mask;

	/* Request IRQ for card detect */
	if ((mmc_slot(host).card_detect_irq)) {
		ret = request_irq(mmc_slot(host).card_detect_irq,
				  omap_hsmmc_cd_handler,
				  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING
					  | IRQF_DISABLED,
				  mmc_hostname(mmc), host);
		if (ret) {
			dev_dbg(mmc_dev(host->mmc),
				"Unable to grab MMC CD IRQ\n");
			goto err_irq_cd;
		}
		pdata->suspend = omap_hsmmc_suspend_cdirq;
		pdata->resume = omap_hsmmc_resume_cdirq;
	} 
#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER)	//20110409	KIMBYUNGCHUL SD_CARD_DETECTION_UPDATE_0409	[START]
		if ((mmc_slot(host).card_detect_irq_by_data3pin)) {
			ret = request_irq(mmc_slot(host).card_detect_irq_by_data3pin,
					  omap_hsmmc_cd_handler,
					  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING
						  | IRQF_DISABLED,
					  mmc_hostname(mmc), host);
			if (ret) {			
				dev_dbg(mmc_dev(host->mmc),
					"Unable to grab MMC CD IRQ\n");
				goto err_irq_cd;
			}
		}
#endif

	else if (mmc_slot(host).mmc_data.register_status_notify) {
		mmc_slot(host).mmc_data.register_status_notify(omap_hsmmc_status_notify_cb, host);
	}
	if (mmc_slot(host).mmc_data.status)
		mmc_slot(host).mmc_data.card_present =
			mmc_slot(host).mmc_data.status(mmc_dev(host->mmc));

	omap_hsmmc_disable_irq(host);

	mmc_host_lazy_disable(host->mmc);

	omap_hsmmc_protect_card(host);

	mmc_add_host(mmc);

	if (mmc_slot(host).name != NULL) {
		ret = device_create_file(&mmc->class_dev, &dev_attr_slot_name);
		if (ret < 0)
			goto err_slot_name;
	}
#if 0
	if (mmc_slot(host).card_detect_irq && mmc_slot(host).get_cover_state) {
		ret = device_create_file(&mmc->class_dev,
					&dev_attr_cover_switch);
		if (ret < 0)
			goto err_slot_name;
	}
#endif

	omap_hsmmc_debugfs(mmc);

	return 0;

err_slot_name:
	mmc_remove_host(mmc);
	free_irq(mmc_slot(host).card_detect_irq, host);
err_irq_cd:
	if (host->use_reg)
		omap_hsmmc_reg_put(host);
err_reg:
	if (host->pdata->cleanup)
		host->pdata->cleanup(&pdev->dev);
err_irq_cd_init:
	free_irq(host->irq, host);
err_irq:
	mmc_host_disable(host->mmc);
	clk_put(host->fclk);
	clk_put(host->iclk);

	if (host->got_dbclk) {
		clk_disable(host->dbclk);
		clk_put(host->dbclk);
	}
err1:
	if (host->adma_table != NULL)
		dma_free_coherent(NULL, ADMA_TABLE_SZ,
			host->adma_table, host->phy_adma_table);
	iounmap(host->base);
err_ioremap:
	platform_set_drvdata(pdev, NULL);
	mmc_free_host(mmc);
err_alloc:
	omap_hsmmc_gpio_free(pdata);
err:
	release_mem_region(res->start, resource_size(res));
	return ret;
}

static int omap_hsmmc_remove(struct platform_device *pdev)
{
	struct omap_hsmmc_host *host = platform_get_drvdata(pdev);
	struct resource *res;

	if (host) {
		mmc_claim_host(host->mmc);
		mmc_remove_host(host->mmc);
		if (host->use_reg)
			omap_hsmmc_reg_put(host);
		if (host->pdata->cleanup)
			host->pdata->cleanup(&pdev->dev);
		free_irq(host->irq, host);
		if (mmc_slot(host).card_detect_irq)
			free_irq(mmc_slot(host).card_detect_irq, host);

#if defined(CONFIG_MACH_LGE_MMC_ENHANCED_COVER) && defined(CONFIG_MACH_LGE_MMC_COVER) //nthyunjin.yang 120517 for sdcard
		if (mmc_slot(host).card_detect_irq_by_data3pin)
			free_irq(mmc_slot(host).card_detect_irq_by_data3pin, host);
#endif

		flush_work_sync(&host->mmc_carddetect_work);

		if (host->adma_table != NULL)
			dma_free_coherent(NULL, ADMA_TABLE_SZ,
				host->adma_table, host->phy_adma_table);

		mmc_release_host(host->mmc);
		pm_runtime_suspend(host->dev);

		clk_put(host->fclk);
		clk_put(host->iclk);
		if (host->got_dbclk) {
			clk_disable(host->dbclk);
			clk_put(host->dbclk);
		}

		mmc_free_host(host->mmc);
		iounmap(host->base);
		omap_hsmmc_gpio_free(pdev->dev.platform_data);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res)
		release_mem_region(res->start, resource_size(res));
	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_PM
static int omap_hsmmc_suspend(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_hsmmc_host *host = platform_get_drvdata(pdev);

	/* LGE_SJIT 2012-01-09 [dojip.kim@lge.com]
	 * no suspend. Some drivers don't use pm on MMC (eg. BRCM WiFi)
	 */
	if (host && mmc_slot(host).no_suspend)
		return 0;

	if (host && host->suspended)
		return 0;

	if (host) {
		pm_runtime_get_sync(host->dev);

		host->suspended = 1;
		if (host->pdata->suspend) {
			ret = host->pdata->suspend(&pdev->dev,
							host->slot_id);
			if (ret) {
				dev_dbg(mmc_dev(host->mmc),
					"Unable to handle MMC board"
					" level suspend\n");
				host->suspended = 0;
				pm_runtime_put_sync(host->dev);
				return ret;
			}
		}
		cancel_work_sync(&host->mmc_carddetect_work);
		if (mmc_slot(host).mmc_data.built_in)
			host->mmc->pm_flags |= MMC_PM_KEEP_POWER;
		ret = mmc_suspend_host(host->mmc);
		if (ret == 0) {
			mmc_claim_host(host->mmc);
			omap_hsmmc_disable_irq(host);
			OMAP_HSMMC_WRITE(host->base, HCTL,
				OMAP_HSMMC_READ(host->base, HCTL) & ~SDBP);
			mmc_release_host_sync(host->mmc);

			if (host->got_dbclk)
				clk_disable(host->dbclk);
		} else {
			host->suspended = 0;
			if (host->pdata->resume) {
				ret = host->pdata->resume(&pdev->dev,
							  host->slot_id);
				if (ret)
					dev_dbg(mmc_dev(host->mmc),
						"Unmask interrupt failed\n");
			}
		}

		pm_runtime_put_sync(host->dev);
	}
	return ret;
}

/* Routine to resume the MMC device */
static int omap_hsmmc_resume(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_hsmmc_host *host = platform_get_drvdata(pdev);

	/* LGE_SJIT 2012-01-09 [dojip.kim@lge.com]
	 * no suspend. Some drivers don't use pm on MMC (eg. BRCM WiFi)
	 */
	if (host && mmc_slot(host).no_suspend)
		return 0;

	if (host && !host->suspended)
		return 0;

	if (host) {
		mmc_claim_host(host->mmc);
		pm_runtime_get_sync(host->dev);

		if (host->got_dbclk)
			clk_enable(host->dbclk);

		omap_hsmmc_conf_bus_power(host);

		if (host->pdata->resume) {
			ret = host->pdata->resume(&pdev->dev, host->slot_id);
			if (ret)
				dev_dbg(mmc_dev(host->mmc),
					"Unmask interrupt failed\n");
		}

		omap_hsmmc_protect_card(host);

		/* Notify the core to resume the host */
		ret = mmc_resume_host(host->mmc);
		if (ret == 0)
			host->suspended = 0;

		mmc_release_host(host->mmc);
		pm_runtime_put_sync(host->dev);
	}

	return ret;
}

#else
#define omap_hsmmc_suspend	NULL
#define omap_hsmmc_resume		NULL
#endif

/* called just before device is disabled */
static int omap_hsmmc_runtime_suspend(struct device *dev)
{
	struct omap_hsmmc_host *host;

	dev_dbg(dev, "%s\n", __func__);

	host = platform_get_drvdata(to_platform_device(dev));
	omap_hsmmc_context_save(host);

	return 0;
}

/* called after device is (re)enabled, ONLY if context was lost */
static int omap_hsmmc_runtime_resume(struct device *dev)
{
	struct omap_hsmmc_host *host;

	dev_dbg(dev, "%s\n", __func__);

	host = platform_get_drvdata(to_platform_device(dev));
	omap_hsmmc_context_restore(host);

	return 0;
}


static struct dev_pm_ops omap_hsmmc_dev_pm_ops = {
	.suspend	= omap_hsmmc_suspend,
	.resume		= omap_hsmmc_resume,
	.runtime_suspend = omap_hsmmc_runtime_suspend,
	.runtime_resume = omap_hsmmc_runtime_resume,
};

static struct platform_driver omap_hsmmc_driver = {
	.remove		= omap_hsmmc_remove,
	.driver		= {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &omap_hsmmc_dev_pm_ops,
	},
};

static int __init omap_hsmmc_init(void)
{
	/* Register the MMC driver */
	return platform_driver_probe(&omap_hsmmc_driver, omap_hsmmc_probe);
}

static void __exit omap_hsmmc_cleanup(void)
{
	/* Unregister MMC driver */
	platform_driver_unregister(&omap_hsmmc_driver);
}

module_init(omap_hsmmc_init);
module_exit(omap_hsmmc_cleanup);

MODULE_DESCRIPTION("OMAP High Speed Multimedia Card driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Texas Instruments Inc");
