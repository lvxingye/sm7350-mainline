// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PCIe MSI doorbell controller
 *
 * On several Qualcomm SoCs the Synopsis DWC iMSI-RX window is not wired
 * to the GIC.  Instead the PCIe host exposes a single MSI doorbell
 * register; a write to that register with a GIC SPI number in the data
 * payload raises one of a contiguous range of GIC SPIs.
 *
 * This is a small, self-contained MSI domain for that doorbell.  It is
 * intentionally limited to the 32-vector layout used by the PCIe0
 * controller on Shima/SM7350.
 */

#include <linux/bitmap.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/slab.h>

#define QCOM_PCI_MSI_MAX_VECTORS	32

struct qcom_pci_msi {
	struct device_node *np;
	struct irq_domain *inner_domain;
	struct irq_domain *msi_domain;
	phys_addr_t msi_addr;
	unsigned int nr_vectors;
	unsigned int parent_irq[QCOM_PCI_MSI_MAX_VECTORS];
	unsigned int parent_hwirq[QCOM_PCI_MSI_MAX_VECTORS];
	DECLARE_BITMAP(bitmap, QCOM_PCI_MSI_MAX_VECTORS);
	struct mutex lock;
};

static struct qcom_pci_msi *qcom_pci_msi_controller;

static void qcom_pci_msi_compose_msg(struct irq_data *data,
				     struct msi_msg *msg)
{
	struct qcom_pci_msi *msi = qcom_pci_msi_controller;
	unsigned int vector = data->hwirq;

	if (!msi || vector >= msi->nr_vectors) {
		msg->address_lo = 0;
		msg->address_hi = 0;
		msg->data = 0;
		return;
	}

	msg->address_lo = lower_32_bits(msi->msi_addr);
	msg->address_hi = upper_32_bits(msi->msi_addr);
	msg->data = msi->parent_hwirq[vector];
}

static void qcom_pci_msi_mask(struct irq_data *data)
{
}

static void qcom_pci_msi_unmask(struct irq_data *data)
{
}

static int qcom_pci_msi_set_affinity(struct irq_data *data,
				     const struct cpumask *mask, bool force)
{
	return IRQ_SET_MASK_OK_NOCOPY;
}

static void qcom_pci_msi_write_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pr_info("qcom-pci-msi: irq %u msg %08x:%08x data %u\n",
		data->irq, msg->address_hi, msg->address_lo, msg->data);

	/*
	 * For MSI only the first interrupt of a multi-MSI allocation owns
	 * the shared address/data pair in the PCI MSI capability.
	 */
	if (desc && desc->irq == data->irq)
		__pci_write_msi_msg(desc, msg);
}

static struct irq_chip qcom_pci_msi_bottom_chip = {
	.name			= "qcom-pci-msi",
	.irq_mask		= qcom_pci_msi_mask,
	.irq_unmask		= qcom_pci_msi_unmask,
	.irq_compose_msi_msg	= qcom_pci_msi_compose_msg,
	.irq_write_msi_msg	= qcom_pci_msi_write_msg,
	.irq_set_affinity	= qcom_pci_msi_set_affinity,
};

static int qcom_pci_msi_domain_alloc(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs,
				     void *args)
{
	struct qcom_pci_msi *msi = domain->host_data;
	unsigned int pos;
	int i;

	if (!nr_irqs || nr_irqs > msi->nr_vectors)
		return -EINVAL;

	mutex_lock(&msi->lock);
	pos = bitmap_find_next_zero_area(msi->bitmap, msi->nr_vectors, 0,
					 nr_irqs, 0);
	if (pos >= msi->nr_vectors) {
		mutex_unlock(&msi->lock);
		return -ENOSPC;
	}
	bitmap_set(msi->bitmap, pos, nr_irqs);
	mutex_unlock(&msi->lock);

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, pos + i,
				    &qcom_pci_msi_bottom_chip, msi,
				    handle_simple_irq, NULL, NULL);

	return 0;
}

static void qcom_pci_msi_domain_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);
	struct qcom_pci_msi *msi = qcom_pci_msi_controller;

	if (!data || !msi)
		return;

	mutex_lock(&msi->lock);
	bitmap_clear(msi->bitmap, data->hwirq, nr_irqs);
	mutex_unlock(&msi->lock);
}

static const struct irq_domain_ops qcom_pci_msi_inner_ops = {
	.alloc	= qcom_pci_msi_domain_alloc,
	.free	= qcom_pci_msi_domain_free,
};

static struct msi_domain_info qcom_pci_msi_domain_info = {
	.flags	= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		  MSI_FLAG_MULTI_PCI_MSI,
	.bus_token = DOMAIN_BUS_PCI_MSI,
	.chip	= &qcom_pci_msi_bottom_chip,
};

static irqreturn_t qcom_pci_msi_irq(int irq, void *dev_id)
{
	unsigned long vector = (unsigned long)dev_id;
	struct qcom_pci_msi *msi = qcom_pci_msi_controller;
	unsigned int virq;

	if (!msi)
		return IRQ_NONE;

	virq = irq_find_mapping(msi->inner_domain, vector);
	if (virq)
		generic_handle_irq(virq);

	return IRQ_HANDLED;
}

static int __init qcom_pci_msi_probe_dt(void)
{
	struct qcom_pci_msi *msi;
	struct resource res;
	struct device_node *np;
	unsigned int i;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,pci-msi");
	if (!np)
		return 0;

	if (!of_property_read_bool(np, "msi-controller")) {
		of_node_put(np);
		return -EINVAL;
	}

	ret = of_address_to_resource(np, 0, &res);
	if (ret || !res.start) {
		of_node_put(np);
		return ret ?: -EINVAL;
	}

	msi = kzalloc(sizeof(*msi), GFP_KERNEL);
	if (!msi) {
		of_node_put(np);
		return -ENOMEM;
	}

	msi->np = np;
	msi->msi_addr = res.start;
	mutex_init(&msi->lock);

	msi->nr_vectors = of_irq_count(np);
	if (msi->nr_vectors > QCOM_PCI_MSI_MAX_VECTORS)
		msi->nr_vectors = QCOM_PCI_MSI_MAX_VECTORS;
	if (!msi->nr_vectors) {
		ret = -EINVAL;
		goto err_free;
	}

	for (i = 0; i < msi->nr_vectors; i++) {
		struct irq_data *irqd;

		msi->parent_irq[i] = irq_of_parse_and_map(np, i);
		if (!msi->parent_irq[i]) {
			ret = -EINVAL;
			goto err_irqs;
		}

		irqd = irq_get_irq_data(msi->parent_irq[i]);
		if (!irqd) {
			ret = -EINVAL;
			goto err_irqs;
		}
		msi->parent_hwirq[i] = irqd->hwirq;
	}

	msi->inner_domain = irq_domain_add_linear(np, msi->nr_vectors,
						  &qcom_pci_msi_inner_ops,
						  msi);
	if (!msi->inner_domain) {
		ret = -ENOMEM;
		goto err_irqs;
	}

	msi->msi_domain = msi_create_irq_domain(of_fwnode_handle(np),
						&qcom_pci_msi_domain_info,
						msi->inner_domain);
	if (!msi->msi_domain) {
		ret = -ENOMEM;
		goto err_domain;
	}

	qcom_pci_msi_controller = msi;

	for (i = 0; i < msi->nr_vectors; i++) {
		ret = request_irq(msi->parent_irq[i], qcom_pci_msi_irq,
				  IRQF_NO_THREAD, "qcom-pci-msi",
				  (void *)(unsigned long)i);
		if (ret)
			goto err_irq_request;
	}

	pr_info("qcom-pci-msi: %u vectors at %pa\n", msi->nr_vectors,
		&msi->msi_addr);
	return 0;

err_irq_request:
	while (i--)
		free_irq(msi->parent_irq[i], (void *)(unsigned long)i);
	qcom_pci_msi_controller = NULL;
	irq_domain_remove(msi->msi_domain);
err_domain:
	irq_domain_remove(msi->inner_domain);
err_irqs:
	while (i--)
		irq_dispose_mapping(msi->parent_irq[i]);
err_free:
	of_node_put(np);
	kfree(msi);
	return ret;
}
arch_initcall(qcom_pci_msi_probe_dt);
