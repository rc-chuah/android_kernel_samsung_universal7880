/*
 * Exynos specific support for Samsung pinctrl/gpiolib driver with eint support.
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 * Copyright (c) 2012 Linaro Ltd
 *		http://www.linaro.org
 *
 * Author: Thomas Abraham <thomas.ab@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This file contains the Samsung Exynos specific information required by the
 * the Samsung pinctrl/gpiolib driver. It also includes the implementation of
 * external gpio and wakeup interrupt support.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/err.h>

#include "pinctrl-samsung.h"
#include "pinctrl-exynos.h"

struct exynos_irq_chip {
	struct irq_chip chip;

	u32 eint_con;
	u32 eint_mask;
	u32 eint_pend;
};

static inline struct exynos_irq_chip *to_exynos_irq_chip(struct irq_chip *chip)
{
	return container_of(chip, struct exynos_irq_chip, chip);
}

/* bank type for non-alive type (DRV bit field: 2) */
static struct samsung_pin_bank_type bank_type_0  = {
	.fld_width = { 4, 1, 2, 2, 2, 2, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, },
};

/* bank type for alive type (DRV bit field: 2) */
static struct samsung_pin_bank_type bank_type_1 = {
	.fld_width = { 4, 1, 2, 2, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, },
};

/* bank type for non-alive type (DRV bit field: 3) */
static struct samsung_pin_bank_type bank_type_4  = {
	.fld_width = { 4, 1, 2, 3, 2, 2, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, },
};

/* bank type for alive type (DRV bit field: 3) */
static struct samsung_pin_bank_type bank_type_5 = {
	.fld_width = { 4, 1, 2, 3, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, },
};

/* list of external wakeup controllers supported */
static const struct of_device_id exynos_wkup_irq_ids[] = {
	{ .compatible = "samsung,exynos4210-wakeup-eint", },
	{ }
};

static void exynos_irq_mask(struct irq_data *irqd)
{
	struct irq_chip *chip = irq_data_get_irq_chip(irqd);
	struct exynos_irq_chip *our_chip = to_exynos_irq_chip(chip);
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	unsigned long reg_mask = our_chip->eint_mask + bank->eint_offset;
	unsigned long mask;
	unsigned long flags;

	spin_lock_irqsave(&bank->slock, flags);

	mask = readl(d->virt_base + reg_mask);
	mask |= 1 << irqd->hwirq;
	writel(mask, d->virt_base + reg_mask);

	spin_unlock_irqrestore(&bank->slock, flags);
}

static void exynos_irq_ack(struct irq_data *irqd)
{
	struct irq_chip *chip = irq_data_get_irq_chip(irqd);
	struct exynos_irq_chip *our_chip = to_exynos_irq_chip(chip);
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	unsigned long reg_pend = our_chip->eint_pend + bank->eint_offset;

	writel(1 << irqd->hwirq, d->virt_base + reg_pend);
}

static void exynos_irq_unmask(struct irq_data *irqd)
{
	struct irq_chip *chip = irq_data_get_irq_chip(irqd);
	struct exynos_irq_chip *our_chip = to_exynos_irq_chip(chip);
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	unsigned long reg_mask = our_chip->eint_mask + bank->eint_offset;
	unsigned long mask;
	unsigned long flags;

	/*
	 * Ack level interrupts right before unmask
	 *
	 * If we don't do this we'll get a double-interrupt.  Level triggered
	 * interrupts must not fire an interrupt if the level is not
	 * _currently_ active, even if it was active while the interrupt was
	 * masked.
	 */
	if (irqd_get_trigger_type(irqd) & IRQ_TYPE_LEVEL_MASK)
		exynos_irq_ack(irqd);

	spin_lock_irqsave(&bank->slock, flags);

	mask = readl(d->virt_base + reg_mask);
	mask &= ~(1 << irqd->hwirq);
	writel(mask, d->virt_base + reg_mask);

	spin_unlock_irqrestore(&bank->slock, flags);
}

static int exynos_irq_set_type(struct irq_data *irqd, unsigned int type)
{
	struct irq_chip *chip = irq_data_get_irq_chip(irqd);
	struct exynos_irq_chip *our_chip = to_exynos_irq_chip(chip);
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	unsigned int shift = EXYNOS_EINT_CON_LEN * irqd->hwirq;
	unsigned int con, trig_type;
	unsigned long reg_con = our_chip->eint_con + bank->eint_offset;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		trig_type = EXYNOS_EINT_EDGE_RISING;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		trig_type = EXYNOS_EINT_EDGE_FALLING;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		trig_type = EXYNOS_EINT_EDGE_BOTH;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		trig_type = EXYNOS_EINT_LEVEL_HIGH;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		trig_type = EXYNOS_EINT_LEVEL_LOW;
		break;
	default:
		pr_err("unsupported external interrupt type\n");
		return -EINVAL;
	}

	if (type & IRQ_TYPE_EDGE_BOTH)
		__irq_set_handler_locked(irqd->irq, handle_edge_irq);
	else
		__irq_set_handler_locked(irqd->irq, handle_level_irq);

	con = readl(d->virt_base + reg_con);
	con &= ~(EXYNOS_EINT_CON_MASK << shift);
	con |= trig_type << shift;
	writel(con, d->virt_base + reg_con);

	return 0;
}

static int exynos_irq_request_resources(struct irq_data *irqd)
{
	struct irq_chip *chip = irq_data_get_irq_chip(irqd);
	struct exynos_irq_chip *our_chip = to_exynos_irq_chip(chip);
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	struct samsung_pin_bank_type *bank_type = bank->type;
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	unsigned int shift = EXYNOS_EINT_CON_LEN * irqd->hwirq;
	unsigned long reg_con = our_chip->eint_con + bank->eint_offset;
	unsigned long flags;
	unsigned int mask;
	unsigned int con;
	int ret;

	ret = gpiochip_lock_as_irq(&bank->gpio_chip, irqd->hwirq);
	if (ret) {
		dev_err(bank->gpio_chip.dev, "unable to lock pin %s-%lu IRQ\n",
			bank->name, irqd->hwirq);
		return ret;
	}

	reg_con = bank->pctl_offset + bank_type->reg_offset[PINCFG_TYPE_FUNC];
	shift = irqd->hwirq * bank_type->fld_width[PINCFG_TYPE_FUNC];
	mask = (1 << bank_type->fld_width[PINCFG_TYPE_FUNC]) - 1;

	spin_lock_irqsave(&bank->slock, flags);

	con = readl(d->virt_base + reg_con);
	con &= ~(mask << shift);
	con |= EXYNOS_EINT_FUNC << shift;
	writel(con, d->virt_base + reg_con);

	spin_unlock_irqrestore(&bank->slock, flags);

	return 0;
}

static void exynos_irq_release_resources(struct irq_data *irqd)
{
	struct irq_chip *chip = irq_data_get_irq_chip(irqd);
	struct exynos_irq_chip *our_chip = to_exynos_irq_chip(chip);
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	struct samsung_pin_bank_type *bank_type = bank->type;
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	unsigned int shift = EXYNOS_EINT_CON_LEN * irqd->hwirq;
	unsigned long reg_con = our_chip->eint_con + bank->eint_offset;
	unsigned long flags;
	unsigned int mask;
	unsigned int con;

	reg_con = bank->pctl_offset + bank_type->reg_offset[PINCFG_TYPE_FUNC];
	shift = irqd->hwirq * bank_type->fld_width[PINCFG_TYPE_FUNC];
	mask = (1 << bank_type->fld_width[PINCFG_TYPE_FUNC]) - 1;

	spin_lock_irqsave(&bank->slock, flags);

	con = readl(d->virt_base + reg_con);
	con &= ~(mask << shift);
	con |= FUNC_INPUT << shift;
	writel(con, d->virt_base + reg_con);

	spin_unlock_irqrestore(&bank->slock, flags);

	gpiochip_unlock_as_irq(&bank->gpio_chip, irqd->hwirq);
}

/*
 * irq_chip for gpio interrupts.
 */
static struct exynos_irq_chip exynos_gpio_irq_chip = {
	.chip = {
		.name = "exynos_gpio_irq_chip",
		.irq_unmask = exynos_irq_unmask,
		.irq_mask = exynos_irq_mask,
		.irq_ack = exynos_irq_ack,
		.irq_set_type = exynos_irq_set_type,
		.irq_request_resources = exynos_irq_request_resources,
		.irq_release_resources = exynos_irq_release_resources,
	},
	.eint_con = EXYNOS_GPIO_ECON_OFFSET,
	.eint_mask = EXYNOS_GPIO_EMASK_OFFSET,
	.eint_pend = EXYNOS_GPIO_EPEND_OFFSET,
};

static int exynos_gpio_irq_map(struct irq_domain *h, unsigned int virq,
					irq_hw_number_t hw)
{
	struct samsung_pin_bank *b = h->host_data;

	irq_set_chip_data(virq, b);
	irq_set_chip_and_handler(virq, &exynos_gpio_irq_chip.chip,
					handle_level_irq);
	set_irq_flags(virq, IRQF_VALID);
	return 0;
}

/*
 * irq domain callbacks for external gpio interrupt controller.
 */
static const struct irq_domain_ops exynos_gpio_irqd_ops = {
	.map	= exynos_gpio_irq_map,
	.xlate	= irq_domain_xlate_twocell,
};

static irqreturn_t exynos_eint_gpio_irq(int irq, void *data)
{
	struct samsung_pinctrl_drv_data *d = data;
	struct samsung_pin_ctrl *ctrl = d->ctrl;
	struct samsung_pin_bank *bank = ctrl->pin_banks;
	unsigned int svc, group, pin, virq;

	svc = readl(d->virt_base + EXYNOS_SVC_OFFSET);
	group = EXYNOS_SVC_GROUP(svc);
	pin = svc & EXYNOS_SVC_NUM_MASK;

	if (!group)
		return IRQ_HANDLED;
	bank += (group - 1);

	virq = irq_linear_revmap(bank->irq_domain, pin);
	if (!virq)
		return IRQ_NONE;
	generic_handle_irq(virq);
	return IRQ_HANDLED;
}

struct exynos_eint_gpio_save {
	u32 eint_con;
	u32 eint_fltcon0;
	u32 eint_fltcon1;
};

static void exynos_eint_flt_config(int en, int sel, int width,
				   struct samsung_pinctrl_drv_data *d,
				   struct samsung_pin_bank *bank)
{
	unsigned int flt_reg, flt_con;
	unsigned int val, shift;
	int i;

	flt_con = 0;

	if (en)
		flt_con |= EXYNOS_EINT_FLTCON_EN;

	if (sel)
		flt_con |= EXYNOS_EINT_FLTCON_SEL;

	flt_con |= EXYNOS_EINT_FLTCON_WIDTH(width);

	flt_reg = EXYNOS_GPIO_EFLTCON_OFFSET + 2 * bank->eint_offset;
	for (i = 0; i < bank->nr_pins >> 1; i++) {
		shift = i * EXYNOS_EINT_FLTCON_LEN;
		val = readl(d->virt_base + flt_reg);
		val &= ~(EXYNOS_EINT_FLTCON_MASK << shift);
		val |= (flt_con << shift);
		writel(val, d->virt_base + flt_reg);
		writel(val, d->virt_base + flt_reg + 0x4);
	}
};

/*
 * exynos_eint_gpio_init() - setup handling of external gpio interrupts.
 * @d: driver data of samsung pinctrl driver.
 */
static int exynos_eint_gpio_init(struct samsung_pinctrl_drv_data *d)
{
	struct samsung_pin_bank *bank;
	struct device *dev = d->dev;
	int ret;
	int i;

	if (!d->irq) {
		dev_err(dev, "irq number not available\n");
		return -EINVAL;
	}

	ret = devm_request_irq(dev, d->irq, exynos_eint_gpio_irq,
					0, dev_name(dev), d);
	if (ret) {
		dev_err(dev, "irq request failed\n");
		return -ENXIO;
	}

	bank = d->ctrl->pin_banks;
	for (i = 0; i < d->ctrl->nr_banks; ++i, ++bank) {
		if (bank->eint_type != EINT_TYPE_GPIO)
			continue;
		bank->irq_domain = irq_domain_add_linear(bank->of_node,
				bank->nr_pins, &exynos_gpio_irqd_ops, bank);
		if (!bank->irq_domain) {
			dev_err(dev, "gpio irq domain add failed\n");
			ret = -ENXIO;
			goto err_domains;
		}

		bank->soc_priv = devm_kzalloc(d->dev,
			sizeof(struct exynos_eint_gpio_save), GFP_KERNEL);
		if (!bank->soc_priv) {
			irq_domain_remove(bank->irq_domain);
			ret = -ENOMEM;
			goto err_domains;
		}
		/* Setting Digital Filter */
		exynos_eint_flt_config(EXYNOS_EINT_FLTCON_EN,
					EXYNOS_EINT_FLTCON_SEL, 0, d, bank);
	}

	return 0;

err_domains:
	for (--i, --bank; i >= 0; --i, --bank) {
		if (bank->eint_type != EINT_TYPE_GPIO)
			continue;
		irq_domain_remove(bank->irq_domain);
	}

	return ret;
}

static u32 exynos_eint_wake_mask = 0xffffffff;

u32 exynos_get_eint_wake_mask(void)
{
	return exynos_eint_wake_mask;
}

static int exynos_wkup_irq_set_wake(struct irq_data *irqd, unsigned int on)
{
	struct samsung_pin_bank *bank = irq_data_get_irq_chip_data(irqd);
	unsigned long bit = 1UL << (2 * bank->eint_offset + irqd->hwirq);

	pr_info("wake %s for irq %d\n", on ? "enabled" : "disabled", irqd->irq);

	if (!on)
		exynos_eint_wake_mask |= bit;
	else
		exynos_eint_wake_mask &= ~bit;

	return 0;
}

/*
 * irq_chip for wakeup interrupts
 */
static struct exynos_irq_chip exynos_wkup_irq_chip = {
	.chip = {
		.name = "exynos_wkup_irq_chip",
		.irq_unmask = exynos_irq_unmask,
		.irq_mask = exynos_irq_mask,
		.irq_ack = exynos_irq_ack,
		.irq_set_type = exynos_irq_set_type,
		.irq_set_wake = exynos_wkup_irq_set_wake,
		.irq_request_resources = exynos_irq_request_resources,
		.irq_release_resources = exynos_irq_release_resources,
	},
	.eint_con = EXYNOS_GPIO_ECON_OFFSET,
	.eint_mask = EXYNOS_GPIO_EMASK_OFFSET,
	.eint_pend = EXYNOS_GPIO_EPEND_OFFSET,
};

/* interrupt handler for wakeup interrupts 0..15 */
static void exynos_irq_eint0_15(unsigned int irq, struct irq_desc *desc)
{
	struct exynos_weint_data *eintd = irq_get_handler_data(irq);
	struct samsung_pin_bank *bank = eintd->bank;
	struct irq_chip *chip = irq_get_chip(irq);
	int eint_irq;

	chained_irq_enter(chip, desc);
	chip->irq_mask(&desc->irq_data);

	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	eint_irq = irq_linear_revmap(bank->irq_domain, eintd->irq);
	generic_handle_irq(eint_irq);
	chip->irq_unmask(&desc->irq_data);
	chained_irq_exit(chip, desc);
}

static inline void exynos_irq_demux_eint(unsigned long pend,
						struct irq_domain *domain)
{
	unsigned int irq;

	while (pend) {
		irq = fls(pend) - 1;
		generic_handle_irq(irq_find_mapping(domain, irq));
		pend &= ~(1 << irq);
	}
}

/* interrupt handler for wakeup interrupt 16 */
static void exynos_irq_demux_eint16_31(unsigned int irq, struct irq_desc *desc)
{
	struct irq_chip *chip = irq_get_chip(irq);
	struct exynos_muxed_weint_data *eintd = irq_get_handler_data(irq);
	struct samsung_pinctrl_drv_data *d = eintd->banks[0]->drvdata;
	unsigned long pend;
	unsigned long mask;
	int i;

	chained_irq_enter(chip, desc);

	for (i = 0; i < eintd->nr_banks; ++i) {
		struct samsung_pin_bank *b = eintd->banks[i];
		pend = readl(d->virt_base + EXYNOS_GPIO_EPEND_OFFSET
				+ b->eint_offset);
		mask = readl(d->virt_base + EXYNOS_GPIO_EMASK_OFFSET
				+ b->eint_offset);
		exynos_irq_demux_eint(pend & ~mask, b->irq_domain);
	}

	chained_irq_exit(chip, desc);
}

static int exynos_wkup_irq_map(struct irq_domain *h, unsigned int virq,
					irq_hw_number_t hw)
{
	irq_set_chip_and_handler(virq, &exynos_wkup_irq_chip.chip,
					handle_level_irq);
	irq_set_chip_data(virq, h->host_data);
	set_irq_flags(virq, IRQF_VALID);
	return 0;
}

/*
 * irq domain callbacks for external wakeup interrupt controller.
 */
static const struct irq_domain_ops exynos_wkup_irqd_ops = {
	.map	= exynos_wkup_irq_map,
	.xlate	= irq_domain_xlate_twocell,
};

/*
 * exynos_eint_wkup_init() - setup handling of external wakeup interrupts.
 * @d: driver data of samsung pinctrl driver.
 */
static int exynos_eint_wkup_init(struct samsung_pinctrl_drv_data *d)
{
	struct device *dev = d->dev;
	struct device_node *wkup_np = NULL;
	struct device_node *np;
	struct samsung_pin_bank *bank;
	struct exynos_weint_data *weint_data;
	struct exynos_muxed_weint_data *muxed_data;
	unsigned int muxed_banks = 0;
	unsigned int i;
	int idx, irq;

	for_each_child_of_node(dev->of_node, np) {
		if (of_match_node(exynos_wkup_irq_ids, np)) {
			wkup_np = np;
			break;
		}
	}
	if (!wkup_np)
		return -ENODEV;

	bank = d->ctrl->pin_banks;
	for (i = 0; i < d->ctrl->nr_banks; ++i, ++bank) {
		if (bank->eint_type != EINT_TYPE_WKUP)
			continue;

		/* Setting Digital Filter */
		exynos_eint_flt_config(EXYNOS_EINT_FLTCON_EN,
				EXYNOS_EINT_FLTCON_SEL, 0, d, bank);

		bank->irq_domain = irq_domain_add_linear(bank->of_node,
				bank->nr_pins, &exynos_wkup_irqd_ops, bank);
		if (!bank->irq_domain) {
			dev_err(dev, "wkup irq domain add failed\n");
			return -ENXIO;
		}

		if (!of_find_property(bank->of_node, "interrupts", NULL)) {
			bank->eint_type = EINT_TYPE_WKUP_MUX;
			++muxed_banks;
			continue;
		}

		weint_data = devm_kzalloc(dev, bank->nr_pins
					* sizeof(*weint_data), GFP_KERNEL);
		if (!weint_data) {
			dev_err(dev, "could not allocate memory for weint_data\n");
			return -ENOMEM;
		}

		for (idx = 0; idx < bank->nr_pins; ++idx) {
			irq = irq_of_parse_and_map(bank->of_node, idx);
			if (!irq) {
				dev_err(dev, "irq number for eint-%s-%d not found\n",
							bank->name, idx);
				continue;
			}
			weint_data[idx].irq = idx;
			weint_data[idx].bank = bank;
			irq_set_handler_data(irq, &weint_data[idx]);
			irq_set_chained_handler(irq, exynos_irq_eint0_15);
		}
	}

	if (!muxed_banks)
		return 0;

	irq = irq_of_parse_and_map(wkup_np, 0);
	if (!irq) {
		dev_err(dev, "irq number for muxed EINTs not found\n");
		return 0;
	}

	muxed_data = devm_kzalloc(dev, sizeof(*muxed_data)
		+ muxed_banks*sizeof(struct samsung_pin_bank *), GFP_KERNEL);
	if (!muxed_data) {
		dev_err(dev, "could not allocate memory for muxed_data\n");
		return -ENOMEM;
	}

	irq_set_chained_handler(irq, exynos_irq_demux_eint16_31);
	irq_set_handler_data(irq, muxed_data);

	bank = d->ctrl->pin_banks;
	idx = 0;
	for (i = 0; i < d->ctrl->nr_banks; ++i, ++bank) {
		if (bank->eint_type != EINT_TYPE_WKUP_MUX)
			continue;

		muxed_data->banks[idx++] = bank;
	}
	muxed_data->nr_banks = muxed_banks;

	return 0;
}

static void exynos_pinctrl_suspend_bank(
				struct samsung_pinctrl_drv_data *drvdata,
				struct samsung_pin_bank *bank)
{
	struct exynos_eint_gpio_save *save = bank->soc_priv;
	void __iomem *regs = drvdata->virt_base;

	save->eint_con = readl(regs + EXYNOS_GPIO_ECON_OFFSET
						+ bank->eint_offset);
	save->eint_fltcon0 = readl(regs + EXYNOS_GPIO_EFLTCON_OFFSET
						+ 2 * bank->eint_offset);
	save->eint_fltcon1 = readl(regs + EXYNOS_GPIO_EFLTCON_OFFSET
						+ 2 * bank->eint_offset + 4);

	pr_debug("%s: save     con %#010x\n", bank->name, save->eint_con);
	pr_debug("%s: save fltcon0 %#010x\n", bank->name, save->eint_fltcon0);
	pr_debug("%s: save fltcon1 %#010x\n", bank->name, save->eint_fltcon1);
}

static void exynos_pinctrl_suspend(struct samsung_pinctrl_drv_data *drvdata)
{
	struct samsung_pin_ctrl *ctrl = drvdata->ctrl;
	struct samsung_pin_bank *bank = ctrl->pin_banks;
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	int i;

	for (i = 0; i < ctrl->nr_banks; ++i, ++bank)
		if (bank->eint_type == EINT_TYPE_GPIO)
			exynos_pinctrl_suspend_bank(drvdata, bank);
		else if (bank->eint_type == EINT_TYPE_WKUP ||
			bank->eint_type == EINT_TYPE_WKUP_MUX) {
			/* Setting Analog Filter */
			exynos_eint_flt_config(EXYNOS_EINT_FLTCON_EN,
					0, 0, d, bank);
		}
}

static void exynos_pinctrl_resume_bank(
				struct samsung_pinctrl_drv_data *drvdata,
				struct samsung_pin_bank *bank)
{
	struct exynos_eint_gpio_save *save = bank->soc_priv;
	void __iomem *regs = drvdata->virt_base;

	pr_debug("%s:     con %#010x => %#010x\n", bank->name,
			readl(regs + EXYNOS_GPIO_ECON_OFFSET
			+ bank->eint_offset), save->eint_con);
	pr_debug("%s: fltcon0 %#010x => %#010x\n", bank->name,
			readl(regs + EXYNOS_GPIO_EFLTCON_OFFSET
			+ 2 * bank->eint_offset), save->eint_fltcon0);
	pr_debug("%s: fltcon1 %#010x => %#010x\n", bank->name,
			readl(regs + EXYNOS_GPIO_EFLTCON_OFFSET
			+ 2 * bank->eint_offset + 4), save->eint_fltcon1);

	writel(save->eint_con, regs + EXYNOS_GPIO_ECON_OFFSET
						+ bank->eint_offset);
	writel(save->eint_fltcon0, regs + EXYNOS_GPIO_EFLTCON_OFFSET
						+ 2 * bank->eint_offset);
	writel(save->eint_fltcon1, regs + EXYNOS_GPIO_EFLTCON_OFFSET
						+ 2 * bank->eint_offset + 4);
}

static void exynos_pinctrl_resume(struct samsung_pinctrl_drv_data *drvdata)
{
	struct samsung_pin_ctrl *ctrl = drvdata->ctrl;
	struct samsung_pin_bank *bank = ctrl->pin_banks;
	struct samsung_pinctrl_drv_data *d = bank->drvdata;
	int i;

	for (i = 0; i < ctrl->nr_banks; ++i, ++bank)
		if (bank->eint_type == EINT_TYPE_GPIO) {
			exynos_pinctrl_resume_bank(drvdata, bank);
		}
		else if (bank->eint_type == EINT_TYPE_WKUP ||
			bank->eint_type == EINT_TYPE_WKUP_MUX) {
			/* Setting Digital Filter */
			exynos_eint_flt_config(EXYNOS_EINT_FLTCON_EN,
					EXYNOS_EINT_FLTCON_SEL, 0, d, bank);
		}
}

/* pin banks of s5pv210 pin-controller */
static struct samsung_pin_bank s5pv210_pin_bank[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpb", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpc0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x080, "gpc1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0a0, "gpd0", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x0c0, "gpd1", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0e0, "gpe0", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x100, "gpe1", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x120, "gpf0", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x140, "gpf1", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x160, "gpf2", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x180, "gpf3", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x1a0, "gpg0", 0x34),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x1c0, "gpg1", 0x38),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x1e0, "gpg2", 0x3c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x200, "gpg3", 0x40),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 7, 0x220, "gpi"),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x240, "gpj0", 0x44),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x260, "gpj1", 0x48),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x280, "gpj2", 0x4c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x2a0, "gpj3", 0x50),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x2c0, "gpj4", 0x54),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x2e0, "mp01"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 4, 0x300, "mp02"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x320, "mp03"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x340, "mp04"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x360, "mp05"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x380, "mp06"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x3a0, "mp07"),
	EXYNOS_PIN_BANK_EINTW(bank_type_0, 8, 0xc00, "gph0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_0, 8, 0xc20, "gph1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_0, 8, 0xc40, "gph2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_0, 8, 0xc60, "gph3", 0x0c),
};

struct samsung_pin_ctrl s5pv210_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= s5pv210_pin_bank,
		.nr_banks	= ARRAY_SIZE(s5pv210_pin_bank),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "s5pv210-gpio-ctrl0",
	},
};

/* pin banks of exynos3250 pin-controller 0 */
static struct samsung_pin_bank exynos3250_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpb",  0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpc0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x080, "gpc1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0a0, "gpd0", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0c0, "gpd1", 0x18),
};

/* pin banks of exynos3250 pin-controller 1 */
static struct samsung_pin_bank exynos3250_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x120, "gpe0"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x140, "gpe1"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 3, 0x180, "gpe2"),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpk0", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x060, "gpk1", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x080, "gpk2", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0c0, "gpl0", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x260, "gpm0", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x280, "gpm1", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x2a0, "gpm2", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x2c0, "gpm3", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x2e0, "gpm4", 0x34),
	EXYNOS_PIN_BANK_EINTW(bank_type_0, 8, 0xc00, "gpx0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc20, "gpx1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc40, "gpx2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc60, "gpx3", 0x0c),
};

/*
 * Samsung pinctrl driver data for Exynos3250 SoC. Exynos3250 SoC includes
 * two gpio/pin-mux/pinconfig controllers.
 */
struct samsung_pin_ctrl exynos3250_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= exynos3250_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos3250_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos3250-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 data */
		.pin_banks	= exynos3250_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos3250_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos3250-gpio-ctrl1",
	},
};

/* pin banks of exynos4210 pin-controller 0 */
static struct samsung_pin_bank exynos4210_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpb", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpc0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x080, "gpc1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0A0, "gpd0", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0C0, "gpd1", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x0E0, "gpe0", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x100, "gpe1", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x120, "gpe2", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x140, "gpe3", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x160, "gpe4", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x180, "gpf0", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x1A0, "gpf1", 0x34),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x1C0, "gpf2", 0x38),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x1E0, "gpf3", 0x3c),
};

/* pin banks of exynos4210 pin-controller 1 */
static struct samsung_pin_bank exynos4210_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpj0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x020, "gpj1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x040, "gpk0", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x060, "gpk1", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x080, "gpk2", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x0A0, "gpk3", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0C0, "gpl0", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 3, 0x0E0, "gpl1", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x100, "gpl2", 0x20),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x120, "gpy0"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 4, 0x140, "gpy1"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x160, "gpy2"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x180, "gpy3"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x1A0, "gpy4"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x1C0, "gpy5"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x1E0, "gpy6"),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC00, "gpx0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC20, "gpx1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC40, "gpx2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC60, "gpx3", 0x0c),
};

/* pin banks of exynos4210 pin-controller 2 */
static struct samsung_pin_bank exynos4210_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 7, 0x000, "gpz"),
};

/*
 * Samsung pinctrl driver data for Exynos4210 SoC. Exynos4210 SoC includes
 * three gpio/pin-mux/pinconfig controllers.
 */
struct samsung_pin_ctrl exynos4210_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= exynos4210_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos4210_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos4210-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 data */
		.pin_banks	= exynos4210_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos4210_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos4210-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 data */
		.pin_banks	= exynos4210_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos4210_pin_banks2),
		.label		= "exynos4210-gpio-ctrl2",
	},
};

/* pin banks of exynos4x12 pin-controller 0 */
static struct samsung_pin_bank exynos4x12_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpb", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpc0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x080, "gpc1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0A0, "gpd0", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0C0, "gpd1", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x180, "gpf0", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x1A0, "gpf1", 0x34),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x1C0, "gpf2", 0x38),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x1E0, "gpf3", 0x3c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x240, "gpj0", 0x40),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x260, "gpj1", 0x44),
};

/* pin banks of exynos4x12 pin-controller 1 */
static struct samsung_pin_bank exynos4x12_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x040, "gpk0", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x060, "gpk1", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x080, "gpk2", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x0A0, "gpk3", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x0C0, "gpl0", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x0E0, "gpl1", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x100, "gpl2", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x260, "gpm0", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x280, "gpm1", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x2A0, "gpm2", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x2C0, "gpm3", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x2E0, "gpm4", 0x34),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x120, "gpy0"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 4, 0x140, "gpy1"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x160, "gpy2"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x180, "gpy3"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x1A0, "gpy4"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x1C0, "gpy5"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x1E0, "gpy6"),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC00, "gpx0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC20, "gpx1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC40, "gpx2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC60, "gpx3", 0x0c),
};

/* pin banks of exynos4x12 pin-controller 2 */
static struct samsung_pin_bank exynos4x12_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x000, "gpz", 0x00),
};

/* pin banks of exynos4x12 pin-controller 3 */
static struct samsung_pin_bank exynos4x12_pin_banks3[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpv0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x020, "gpv1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpv2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x060, "gpv3", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x080, "gpv4", 0x10),
};

/*
 * Samsung pinctrl driver data for Exynos4x12 SoC. Exynos4x12 SoC includes
 * four gpio/pin-mux/pinconfig controllers.
 */
struct samsung_pin_ctrl exynos4x12_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= exynos4x12_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos4x12_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos4x12-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 data */
		.pin_banks	= exynos4x12_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos4x12_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos4x12-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 data */
		.pin_banks	= exynos4x12_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos4x12_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos4x12-gpio-ctrl2",
	}, {
		/* pin-controller instance 3 data */
		.pin_banks	= exynos4x12_pin_banks3,
		.nr_banks	= ARRAY_SIZE(exynos4x12_pin_banks3),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos4x12-gpio-ctrl3",
	},
};

/* pin banks of exynos5250 pin-controller 0 */
static struct samsung_pin_bank exynos5250_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpa2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpb0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x080, "gpb1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0A0, "gpb2", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0C0, "gpb3", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x0E0, "gpc0", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x100, "gpc1", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x120, "gpc2", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x140, "gpc3", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x160, "gpd0", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x180, "gpd1", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x2E0, "gpc4", 0x34),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x1A0, "gpy0"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 4, 0x1C0, "gpy1"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x1E0, "gpy2"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x200, "gpy3"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x220, "gpy4"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x240, "gpy5"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x260, "gpy6"),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC00, "gpx0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC20, "gpx1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC40, "gpx2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC60, "gpx3", 0x0c),
};

/* pin banks of exynos5250 pin-controller 1 */
static struct samsung_pin_bank exynos5250_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpe0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x020, "gpe1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x040, "gpf0", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x060, "gpf1", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x080, "gpg0", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0A0, "gpg1", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x0C0, "gpg2", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0E0, "gph0", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x100, "gph1", 0x20),
};

/* pin banks of exynos5250 pin-controller 2 */
static struct samsung_pin_bank exynos5250_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpv0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x020, "gpv1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x060, "gpv2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x080, "gpv3", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x0C0, "gpv4", 0x10),
};

/* pin banks of exynos5250 pin-controller 3 */
static struct samsung_pin_bank exynos5250_pin_banks3[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x000, "gpz", 0x00),
};

/*
 * Samsung pinctrl driver data for Exynos5250 SoC. Exynos5250 SoC includes
 * four gpio/pin-mux/pinconfig controllers.
 */
struct samsung_pin_ctrl exynos5250_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= exynos5250_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos5250_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos5250-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 data */
		.pin_banks	= exynos5250_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos5250_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos5250-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 data */
		.pin_banks	= exynos5250_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos5250_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos5250-gpio-ctrl2",
	}, {
		/* pin-controller instance 3 data */
		.pin_banks	= exynos5250_pin_banks3,
		.nr_banks	= ARRAY_SIZE(exynos5250_pin_banks3),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos5250-gpio-ctrl3",
	},
};

/* pin banks of exynos5260 pin-controller 0 */
static struct samsung_pin_bank exynos5260_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpa2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpb0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x080, "gpb1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x0a0, "gpb2", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0c0, "gpb3", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0e0, "gpb4", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x100, "gpb5", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x120, "gpd0", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x140, "gpd1", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x160, "gpd2", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x180, "gpe0", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x1a0, "gpe1", 0x34),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x1c0, "gpf0", 0x38),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x1e0, "gpf1", 0x3c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x200, "gpk0", 0x40),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc00, "gpx0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc20, "gpx1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc40, "gpx2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xc60, "gpx3", 0x0c),
};

/* pin banks of exynos5260 pin-controller 1 */
static struct samsung_pin_bank exynos5260_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x000, "gpc0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x020, "gpc1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x040, "gpc2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x060, "gpc3", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x080, "gpc4", 0x10),
};

/* pin banks of exynos5260 pin-controller 2 */
static struct samsung_pin_bank exynos5260_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x000, "gpz0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x020, "gpz1", 0x04),
};

/*
 * Samsung pinctrl driver data for Exynos5260 SoC. Exynos5260 SoC includes
 * three gpio/pin-mux/pinconfig controllers.
 */
struct samsung_pin_ctrl exynos5260_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= exynos5260_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos5260_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.label		= "exynos5260-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 data */
		.pin_banks	= exynos5260_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos5260_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos5260-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 data */
		.pin_banks	= exynos5260_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos5260_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos5260-gpio-ctrl2",
	},
};

/* pin banks of exynos5420 pin-controller 0 */
static struct samsung_pin_bank exynos5420_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_1, 8, 0x000, "gpy7", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC00, "gpx0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC20, "gpx1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC40, "gpx2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_1, 8, 0xC60, "gpx3", 0x0c),
};

/* pin banks of exynos5420 pin-controller 1 */
static struct samsung_pin_bank exynos5420_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpc0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x020, "gpc1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x040, "gpc2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x060, "gpc3", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x080, "gpc4", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0A0, "gpd1", 0x14),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x0C0, "gpy0"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 4, 0x0E0, "gpy1"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 6, 0x100, "gpy2"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x120, "gpy3"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x140, "gpy4"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x160, "gpy5"),
	EXYNOS_PIN_BANK_EINTN(bank_type_0, 8, 0x180, "gpy6"),
};

/* pin banks of exynos5420 pin-controller 2 */
static struct samsung_pin_bank exynos5420_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpe0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x020, "gpe1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x040, "gpf0", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x060, "gpf1", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x080, "gpg0", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0A0, "gpg1", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x0C0, "gpg2", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0E0, "gpj4", 0x1c),
};

/* pin banks of exynos5420 pin-controller 3 */
static struct samsung_pin_bank exynos5420_pin_banks3[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 6, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x040, "gpa2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x060, "gpb0", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 5, 0x080, "gpb1", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 4, 0x0A0, "gpb2", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x0C0, "gpb3", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 2, 0x0E0, "gpb4", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 8, 0x100, "gph0", 0x20),
};

/* pin banks of exynos5420 pin-controller 4 */
static struct samsung_pin_bank exynos5420_pin_banks4[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_0, 7, 0x000, "gpz", 0x00),
};

/*
 * Samsung pinctrl driver data for Exynos5420 SoC. Exynos5420 SoC includes
 * four gpio/pin-mux/pinconfig controllers.
 */
struct samsung_pin_ctrl exynos5420_pin_ctrl[] = {
	{
		/* pin-controller instance 0 data */
		.pin_banks	= exynos5420_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos5420_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.label		= "exynos5420-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 data */
		.pin_banks	= exynos5420_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos5420_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos5420-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 data */
		.pin_banks	= exynos5420_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos5420_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos5420-gpio-ctrl2",
	}, {
		/* pin-controller instance 3 data */
		.pin_banks	= exynos5420_pin_banks3,
		.nr_banks	= ARRAY_SIZE(exynos5420_pin_banks3),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos5420-gpio-ctrl3",
	}, {
		/* pin-controller instance 4 data */
		.pin_banks	= exynos5420_pin_banks4,
		.nr_banks	= ARRAY_SIZE(exynos5420_pin_banks4),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos5420-gpio-ctrl4",
	},
};

/* pin banks of exynos8890 pin-controller 0 (ALIVE) */
static struct samsung_pin_bank exynos8890_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x000, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x020, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x040, "gpa2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x060, "gpa3", 0x0c),
};

/* pin banks of exynos8890 pin-controller 1 (AUD) */
static struct samsung_pin_bank exynos8890_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x000, "gph0", 0x00),
};

/* pin banks of exynos8890 pin-controller 2 (CCORE) */
static struct samsung_pin_bank exynos8890_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x000, "etc0", 0x00),
};

/* pin banks of exynos8890 pin-controller 3 (ESE) */
static struct samsung_pin_bank exynos8890_pin_banks3[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x000, "gpf3", 0x00),
};

/* pin banks of exynos8890 pin-controller 4 (FP) */
static struct samsung_pin_bank exynos8890_pin_banks4[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpf2", 0x00),
};

/* pin banks of exynos8890 pin-controller 5 (FSYS0) */
static struct samsung_pin_bank exynos8890_pin_banks5[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpi1", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x020, "gpi2", 0x04),
};

/* pin banks of exynos8890 pin-controller 6 (FSYS1) */
static struct samsung_pin_bank exynos8890_pin_banks6[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x000, "gpj0", 0x00),
};

/* pin banks of exynos8890 pin-controller 7 (NFC) */
static struct samsung_pin_bank exynos8890_pin_banks7[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x000, "gpf0", 0x00),
};

/* pin banks of exynos8890 pin-controller 8 (PERIC0) */
static struct samsung_pin_bank exynos8890_pin_banks8[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x000, "gpi0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x020, "gpd0", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x040, "gpd1", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x060, "gpd2", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x080, "gpd3", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x0A0, "gpb1", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x0C0, "gpb2", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x0E0, "gpb0", 0x1C),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x100, "gpc0", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x120, "gpc1", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x140, "gpc2", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x160, "gpc3", 0x2C),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x180, "gpk0", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x1A0, "etc1", 0x34),
};

/* pin banks of exynos8890 pin-controller 9 (PERIC1) */
static struct samsung_pin_bank exynos8890_pin_banks9[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x000, "gpe0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x020, "gpe5", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x040, "gpe6", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x060, "gpj1", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x080, "gpj2", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x0A0, "gpe2", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x0C0, "gpe3", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x0E0, "gpe4", 0x1C),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x100, "gpe1", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x120, "gpe7", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x140, "gpg0", 0x28),
};

/* pin banks of exynos8890 pin-controller 10 (TOUCH) */
static struct samsung_pin_bank exynos8890_pin_banks10[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x000, "gpf1", 0x00),
};

struct samsung_pin_ctrl exynos8890_pin_ctrl[] = {
	{
		/* pin-controller instance 0 Alive data */
		.pin_banks	= exynos8890_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 AUD data */
		.pin_banks	= exynos8890_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks1),
		.label		= "exynos8890-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 CCORE data */
		.pin_banks	= exynos8890_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl2",
	}, {
		/* pin-controller instance 3 ESE data */
		.pin_banks	= exynos8890_pin_banks3,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks3),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl3",
	}, {
		/* pin-controller instance 4 FP data */
		.pin_banks	= exynos8890_pin_banks4,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks4),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl4",
	}, {
		/* pin-controller instance 5 FSYS0 data */
		.pin_banks	= exynos8890_pin_banks5,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks5),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl5",
	}, {
		/* pin-controller instance 6 FSYS1 data */
		.pin_banks	= exynos8890_pin_banks6,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks6),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl6",
	}, {
		/* pin-controller instance 7 NFC data */
		.pin_banks	= exynos8890_pin_banks7,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks7),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl7",
	}, {
		/* pin-controller instance 8 PERIC0 data */
		.pin_banks	= exynos8890_pin_banks8,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks8),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl8",
	}, {
		/* pin-controller instance 9 PERIC1 data */
		.pin_banks	= exynos8890_pin_banks9,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks9),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl9",
	}, {
		/* pin-controller instance 10 TOUCH data */
		.pin_banks	= exynos8890_pin_banks10,
		.nr_banks	= ARRAY_SIZE(exynos8890_pin_banks10),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos8890-gpio-ctrl10",
	},
};

/* pin banks of exynos7870 pin-controller 0 (ALIVE) */
static struct samsung_pin_bank exynos7870_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 6, 0x000, "etc0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 3, 0x020, "etc1", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x040, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x060, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x080, "gpa2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 2, 0x0c0, "gpq0", 0x00),
};

/* pin banks of exynos7870 pin-controller 1 (DISPAUD) */
static struct samsung_pin_bank exynos7870_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpz0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x020, "gpz1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x040, "gpz2", 0x08),
};

/* pin banks of exynos7870 pin-controller 2 (EsE) */
static struct samsung_pin_bank exynos7870_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x000, "gpc7", 0x00),
};

/* pin banks of exynos7870 pin-controller 3 (FSYS) */
static struct samsung_pin_bank exynos7870_pin_banks3[] = {
         EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x000, "gpr0", 0x00),
         EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x020, "gpr1", 0x04),
         EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x040, "gpr2", 0x08),
         EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x060, "gpr3", 0x0c),
         EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x080, "gpr4", 0x10),
};

/* pin banks of exynos7870 pin-controller 4 (MIF) */
static struct samsung_pin_bank exynos7870_pin_banks4[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x000, "gpm0", 0x00),
};

/* pin banks of exynos7870 pin-controller 5 (NFC) */
static struct samsung_pin_bank exynos7870_pin_banks5[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpc2", 0x00),
};

/* pin banks of exynos7870 pin-controller 6 (TOP) */
static struct samsung_pin_bank exynos7870_pin_banks6[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpb0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x020, "gpc0", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x040, "gpc1", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x060, "gpc4", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x080, "gpc5", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x0a0, "gpc6", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x0c0, "gpc8", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x0e0, "gpc9", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x100, "gpd1", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x120, "gpd2", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x140, "gpd3", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x160, "gpd4", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x1a0, "gpe0", 0x34),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x1c0, "gpf0", 0x38),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x1e0, "gpf1", 0x3c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x200, "gpf2", 0x40),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x220, "gpf3", 0x44),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x240, "gpf4", 0x48),
};

/* pin banks of exynos7870 pin-controller 10 (TOUCH) */
static struct samsung_pin_bank exynos7870_pin_banks7[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x000, "gpc3", 0x00),
};

struct samsung_pin_ctrl exynos7870_pin_ctrl[] = {
	{
		/* pin-controller instance 0 Alive data */
		.pin_banks	= exynos7870_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 DISPAUD data */
		.pin_banks	= exynos7870_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks1),
		.label		= "exynos7870-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 ESE  data */
		.pin_banks	= exynos7870_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl2",
	}, {
		/* pin-controller instance 3 FSYS data */
		.pin_banks	= exynos7870_pin_banks3,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks3),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl3",
	}, {
		/* pin-controller instance 4 MIF data */
		.pin_banks	= exynos7870_pin_banks4,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks4),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl4",
	}, {
		/* pin-controller instance 5 NFC data */
		.pin_banks	= exynos7870_pin_banks5,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks5),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl5",
	}, {
		/* pin-controller instance 6 TOP data */
		.pin_banks	= exynos7870_pin_banks6,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks6),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl6",
	}, {
		/* pin-controller instance 7 TOUCH data */
		.pin_banks	= exynos7870_pin_banks7,
		.nr_banks	= ARRAY_SIZE(exynos7870_pin_banks7),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7870-gpio-ctrl7",
	},
};

/* pin banks of exynos7880 pin-controller 0 (ALIVE) */
static struct samsung_pin_bank exynos7880_pin_banks0[] = {
	EXYNOS_PIN_BANK_EINTN(bank_type_5, 6, 0x000, "etc0"),
	EXYNOS_PIN_BANK_EINTN(bank_type_5, 3, 0x020, "etc1"),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x040, "gpa0", 0x00),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x060, "gpa1", 0x04),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 8, 0x080, "gpa2", 0x08),
	EXYNOS_PIN_BANK_EINTW(bank_type_5, 5, 0x0a0, "gpa3", 0x0c),
	EXYNOS_PIN_BANK_EINTN(bank_type_5, 2, 0x0c0, "gpq0"),
};

/* pin banks of exynos7880 pin-controller 1 (CCORE) */
static struct samsung_pin_bank exynos7880_pin_banks1[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x000, "gpm0", 0x00),
};

/* pin banks of exynos7880 pin-controller 2 (DISPAUD) */
static struct samsung_pin_bank exynos7880_pin_banks2[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpz0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x020, "gpz1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x040, "gpz2", 0x08),
};


/* pin banks of exynos7880 pin-controller 3 (ESE) */
static struct samsung_pin_bank exynos7880_pin_banks3[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x000, "gpc7", 0x00),
};

/* pin banks of exynos7880 pin-controller 4 (FSYS) */
static struct samsung_pin_bank exynos7880_pin_banks4[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x000, "gpr0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x020, "gpr1", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x040, "gpr2", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x060, "gpr3", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x080, "gpr4", 0x10),
};

/* pin banks of exynos7880 pin-controller 5 (NFC) */
static struct samsung_pin_bank exynos7880_pin_banks5[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpc2", 0x00),
};

/* pin banks of exynos7880 pin-controller 6 (TOP) */
static struct samsung_pin_bank exynos7880_pin_banks6[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x000, "gpb0", 0x00),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x020, "gpc0", 0x04),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x040, "gpc1", 0x08),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x060, "gpc4", 0x0c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x080, "gpc5", 0x10),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x0a0, "gpc6", 0x14),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x0c0, "gpc8", 0x18),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x0e0, "gpc9", 0x1c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x100, "gpd1", 0x20),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 6, 0x120, "gpd2", 0x24),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 8, 0x140, "gpd3", 0x28),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 7, 0x160, "gpd4", 0x2c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x180, "gpd5", 0x30),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x1a0, "gpe0", 0x34),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x1c0, "gpf0", 0x38),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x1e0, "gpf1", 0x3c),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 2, 0x200, "gpf2", 0x40),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 4, 0x220, "gpf3", 0x44),
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 5, 0x240, "gpf4", 0x48),
};

/* pin banks of exynos7880 pin-controller 7 (TOUCH) */
static struct samsung_pin_bank exynos7880_pin_banks7[] = {
	EXYNOS_PIN_BANK_EINTG(bank_type_4, 3, 0x000, "gpc3", 0x00),
};

struct samsung_pin_ctrl exynos7880_pin_ctrl[] = {
	{
		/* pin-controller instance 0 Alive data */
		.pin_banks	= exynos7880_pin_banks0,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks0),
		.eint_gpio_init = exynos_eint_gpio_init,
		.eint_wkup_init = exynos_eint_wkup_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7880-gpio-ctrl0",
	}, {
		/* pin-controller instance 1 CCORE data */
		.pin_banks	= exynos7880_pin_banks1,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks1),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7880-gpio-ctrl1",
	}, {
		/* pin-controller instance 2 DISPAUD data */
		.pin_banks	= exynos7880_pin_banks2,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
		.label		= "exynos7880-gpio-ctrl2",
	}, {
		/* pin-controller instance 3 ESE data */
		.pin_banks	= exynos7880_pin_banks3,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks3),
#ifndef ENABLE_SENSORS_FPRINT_SECURE
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
#endif
		.label		= "exynos7880-gpio-ctrl3",
	}, {
		/* pin-controller instance 4 FSYS data */
		.pin_banks	= exynos7880_pin_banks4,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks4),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7880-gpio-ctrl4",
	}, {
		/* pin-controller instance 5 NFC data */
		.pin_banks	= exynos7880_pin_banks5,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks5),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7880-gpio-ctrl5",
	}, {
		/* pin-controller instance 6 TOP data */
		.pin_banks	= exynos7880_pin_banks6,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks6),
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
		.label		= "exynos7880-gpio-ctrl6",
	}, {
		/* pin-controller instance 7 TOUCH data */
		.pin_banks	= exynos7880_pin_banks7,
		.nr_banks	= ARRAY_SIZE(exynos7880_pin_banks7),
#ifndef CONFIG_MST_SECURE_GPIO
		.eint_gpio_init = exynos_eint_gpio_init,
		.suspend	= exynos_pinctrl_suspend,
		.resume		= exynos_pinctrl_resume,
#endif
		.label		= "exynos7880-gpio-ctrl7",
	},
};

#ifdef CONFIG_SEC_GPIO_DVS
int exynos7880_secgpio_get_nr_gpio(void)
{
	int i, j;
	int nr_gpio = 0;

	for (i = 0; i < ARRAY_SIZE(exynos7880_pin_ctrl); i++) {
		for(j = 0; j < exynos7880_pin_ctrl[i].nr_banks; j++)
			nr_gpio += exynos7880_pin_ctrl[i].pin_banks[j].nr_pins;
	}

	return nr_gpio;
}
#endif

#if defined(CONFIG_SOC_EXYNOS7870)
u32 exynos_eint_to_pin_num(int eint)
{
	return exynos7870_pin_ctrl[0].base + eint;
}
#endif

#if defined(CONFIG_SOC_EXYNOS8890)
u32 exynos_eint_to_pin_num(int eint)
{
	return exynos8890_pin_ctrl[0].base + eint;
}
#endif

#if defined(CONFIG_SOC_EXYNOS7880)
u32 exynos_eint_to_pin_num(int eint)
{
	int i;
	int etc_offset = 0;

	for(i = 0; i < exynos7880_pin_ctrl[0].nr_banks &&
		strncmp(exynos7880_pin_ctrl[0].pin_banks[i].name, "gpa", 3); i++)
		etc_offset += exynos7870_pin_ctrl[0].pin_banks[i].nr_pins;

        return exynos7880_pin_ctrl[0].base + eint + etc_offset;
}
#endif
