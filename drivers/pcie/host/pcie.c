/*
 * Copyright (c) 2019 Intel Corporation
 * Copyright (c) 2020 acontis technologies GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pcie, LOG_LEVEL_ERR);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdbool.h>
#include <zephyr/drivers/pcie/pcie.h>

#if CONFIG_PCIE_MSI
#include <zephyr/drivers/pcie/msi.h>
#endif

#ifdef CONFIG_PCIE_CONTROLLER
#include <zephyr/drivers/pcie/controller.h>
#endif

/* functions documented in drivers/pcie/pcie.h */

bool pcie_probe(pcie_bdf_t bdf, pcie_id_t id)
{
	uint32_t data;

	data = pcie_conf_read(bdf, PCIE_CONF_ID);

	if (data == PCIE_ID_NONE) {
		return false;
	}

	if (id == PCIE_ID_NONE) {
		return true;
	}

	return (id == data);
}

void pcie_set_cmd(pcie_bdf_t bdf, uint32_t bits, bool on)
{
	uint32_t cmdstat;

	cmdstat = pcie_conf_read(bdf, PCIE_CONF_CMDSTAT);

	if (on) {
		cmdstat |= bits;
	} else {
		cmdstat &= ~bits;
	}

	pcie_conf_write(bdf, PCIE_CONF_CMDSTAT, cmdstat);
}

uint32_t pcie_get_cap(pcie_bdf_t bdf, uint32_t cap_id)
{
	uint32_t reg = 0U;
	uint32_t data;

	data = pcie_conf_read(bdf, PCIE_CONF_CMDSTAT);
	if ((data & PCIE_CONF_CMDSTAT_CAPS) != 0U) {
		data = pcie_conf_read(bdf, PCIE_CONF_CAPPTR);
		reg = PCIE_CONF_CAPPTR_FIRST(data);
	}

	while (reg != 0U) {
		data = pcie_conf_read(bdf, reg);

		if (PCIE_CONF_CAP_ID(data) == cap_id) {
			break;
		}

		reg = PCIE_CONF_CAP_NEXT(data);
	}

	return reg;
}

uint32_t pcie_get_ext_cap(pcie_bdf_t bdf, uint32_t cap_id)
{
	unsigned int reg = PCIE_CONF_EXT_CAPPTR; /* Start at end of the PCI configuration space */
	uint32_t data;

	while (reg != 0U) {
		data = pcie_conf_read(bdf, reg);
		if (!data || data == 0xffffffffU) {
			return 0;
		}

		if (PCIE_CONF_EXT_CAP_ID(data) == cap_id) {
			break;
		}

		reg = PCIE_CONF_EXT_CAP_NEXT(data) >> 2;

		if (reg < PCIE_CONF_EXT_CAPPTR) {
			return 0;
		}
	}

	return reg;
}

/**
 * @brief Get the BAR at a specific BAR index
 *
 * @param bdf the PCI(e) endpoint
 * @param bar_index 0-based BAR index
 * @param bar Pointer to struct pcie_bar
 * @param io true for I/O BARs, false otherwise
 * @return true if the BAR was found and is valid, false otherwise
 */
static bool pcie_get_bar(pcie_bdf_t bdf,
			 unsigned int bar_index,
			 struct pcie_bar *bar,
			 bool io)
{
	uint32_t reg = bar_index + PCIE_CONF_BAR0;
#ifdef CONFIG_PCIE_CONTROLLER
	const struct device *dev;
#endif
	uintptr_t phys_addr;
	size_t size;

#ifdef CONFIG_PCIE_CONTROLLER
	dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_pcie_controller));
	if (!dev) {
		LOG_ERR("Failed to get PCIe root complex");
		return false;
	}
#endif

	if (reg > PCIE_CONF_BAR5) {
		return false;
	}

	phys_addr = pcie_conf_read(bdf, reg);
#ifndef CONFIG_PCIE_CONTROLLER
	if ((PCIE_CONF_BAR_MEM(phys_addr) && io) || (PCIE_CONF_BAR_IO(phys_addr) && !io)) {
		return false;
	}
#endif

	if (PCIE_CONF_BAR_INVAL_FLAGS(phys_addr)) {
		/* Discard on invalid flags */
		return false;
	}

	pcie_conf_write(bdf, reg, 0xFFFFFFFFU);
	size = pcie_conf_read(bdf, reg);
	pcie_conf_write(bdf, reg, (uint32_t)phys_addr);

	if (IS_ENABLED(CONFIG_64BIT) && PCIE_CONF_BAR_64(phys_addr)) {
		reg++;
		phys_addr |= ((uint64_t)pcie_conf_read(bdf, reg)) << 32;

		if (PCIE_CONF_BAR_ADDR(phys_addr) == PCIE_CONF_BAR_INVAL64 ||
		    PCIE_CONF_BAR_ADDR(phys_addr) == PCIE_CONF_BAR_NONE) {
			/* Discard on invalid address */
			return false;
		}

		pcie_conf_write(bdf, reg, 0xFFFFFFFFU);
		size |= ((uint64_t)pcie_conf_read(bdf, reg)) << 32;
		pcie_conf_write(bdf, reg, (uint32_t)((uint64_t)phys_addr >> 32));
	} else if (PCIE_CONF_BAR_ADDR(phys_addr) == PCIE_CONF_BAR_INVAL ||
		   PCIE_CONF_BAR_ADDR(phys_addr) == PCIE_CONF_BAR_NONE) {
		/* Discard on invalid address */
		return false;
	}

	if (PCIE_CONF_BAR_IO(phys_addr)) {
		size = PCIE_CONF_BAR_IO_ADDR(size);
		if (size == 0) {
			/* Discard on invalid size */
			return false;
		}
	} else {
		size = PCIE_CONF_BAR_ADDR(size);
		if (size == 0) {
			/* Discard on invalid size */
			return false;
		}
	}

#ifdef CONFIG_PCIE_CONTROLLER
	/* Translate to physical memory address from bus address */
	if (!pcie_ctrl_region_translate(dev, bdf, PCIE_CONF_BAR_MEM(phys_addr),
					PCIE_CONF_BAR_64(phys_addr),
					PCIE_CONF_BAR_MEM(phys_addr) ?
						PCIE_CONF_BAR_ADDR(phys_addr)
						: PCIE_CONF_BAR_IO_ADDR(phys_addr),
					&bar->phys_addr)) {
		return false;
	}
#else
	bar->phys_addr = PCIE_CONF_BAR_ADDR(phys_addr);
#endif /* CONFIG_PCIE_CONTROLLER */
	bar->size = size & ~(size-1);

	return true;
}

/**
 * @brief Probe the nth BAR assigned to an endpoint.
 *
 * A PCI(e) endpoint has 0 or more BARs. This function
 * allows the caller to enumerate them by calling with index=0..n.
 * Value of n has to be below 6, as there is a maximum of 6 BARs. The indices
 * are order-preserving with respect to the endpoint BARs: e.g., index 0
 * will return the lowest-numbered BAR on the endpoint.
 *
 * @param bdf the PCI(e) endpoint
 * @param index (0-based) index
 * @param bar Pointer to struct pcie_bar
 * @param io true for I/O BARs, false otherwise
 * @return true if the BAR was found and is valid, false otherwise
 */
static bool pcie_probe_bar(pcie_bdf_t bdf,
			   unsigned int index,
			   struct pcie_bar *bar,
			   bool io)
{
	uint32_t reg;

	for (reg = PCIE_CONF_BAR0;
	     index > 0 && reg <= PCIE_CONF_BAR5; reg++, index--) {
		uintptr_t addr = pcie_conf_read(bdf, reg);

		if (PCIE_CONF_BAR_MEM(addr) && PCIE_CONF_BAR_64(addr)) {
			reg++;
		}
	}

	if (index != 0) {
		return false;
	}

	return pcie_get_bar(bdf, reg - PCIE_CONF_BAR0, bar, io);
}

bool pcie_get_mbar(pcie_bdf_t bdf,
		   unsigned int bar_index,
		   struct pcie_bar *mbar)
{
	return pcie_get_bar(bdf, bar_index, mbar, false);
}

bool pcie_probe_mbar(pcie_bdf_t bdf,
		     unsigned int index,
		     struct pcie_bar *mbar)
{
	return pcie_probe_bar(bdf, index, mbar, false);
}

bool pcie_get_iobar(pcie_bdf_t bdf,
		    unsigned int bar_index,
		    struct pcie_bar *iobar)
{
	return pcie_get_bar(bdf, bar_index, iobar, true);
}

bool pcie_probe_iobar(pcie_bdf_t bdf,
		      unsigned int index,
		      struct pcie_bar *iobar)
{
	return pcie_probe_bar(bdf, index, iobar, true);
}

#ifndef CONFIG_PCIE_CONTROLLER

unsigned int pcie_alloc_irq(pcie_bdf_t bdf)
{
	unsigned int irq;
	uint32_t data;

	data = pcie_conf_read(bdf, PCIE_CONF_INTR);
	irq = PCIE_CONF_INTR_IRQ(data);

	if (irq == PCIE_CONF_INTR_IRQ_NONE ||
	    irq >= CONFIG_MAX_IRQ_LINES ||
	    arch_irq_is_used(irq)) {
		irq = arch_irq_allocate();
		if (irq == UINT_MAX) {
			return PCIE_CONF_INTR_IRQ_NONE;
		}

		data &= ~0xffU;
		data |= irq;
		pcie_conf_write(bdf, PCIE_CONF_INTR, data);
	} else {
		arch_irq_set_used(irq);
	}

	return irq;
}
#endif /* CONFIG_PCIE_CONTROLLER */

unsigned int pcie_get_irq(pcie_bdf_t bdf)
{
	uint32_t data = pcie_conf_read(bdf, PCIE_CONF_INTR);

	return PCIE_CONF_INTR_IRQ(data);
}

bool pcie_connect_dynamic_irq(pcie_bdf_t bdf,
			      unsigned int irq,
			      unsigned int priority,
			      void (*routine)(const void *parameter),
			      const void *parameter,
			      uint32_t flags)
{
#if defined(CONFIG_PCIE_MSI) && defined(CONFIG_PCIE_MSI_MULTI_VECTOR)
	if (pcie_is_msi(bdf)) {
		msi_vector_t vector;

		if ((pcie_msi_vectors_allocate(bdf, priority,
					       &vector, 1) == 0) ||
		    !pcie_msi_vector_connect(bdf, &vector,
					     routine, parameter, flags)) {
			return false;
		}
	} else
#endif /* CONFIG_PCIE_MSI && CONFIG_PCIE_MSI_MULTI_VECTOR */
	{
		if (irq_connect_dynamic(irq, priority, routine,
					parameter, flags) < 0) {
			return false;
		}
	}

	return true;
}

void pcie_irq_enable(pcie_bdf_t bdf, unsigned int irq)
{
#if CONFIG_PCIE_MSI
	if (pcie_msi_enable(bdf, NULL, 1, irq)) {
		return;
	}
#endif
	irq_enable(irq);
}

pcie_bdf_t pcie_bdf_lookup(pcie_id_t id)
{
	int bus, dev, func;

	for (bus = 0; bus <= PCIE_MAX_BUS; bus++) {
		for (dev = 0; dev <= PCIE_MAX_DEV; dev++) {
			for (func = 0; func <= PCIE_MAX_FUNC; func++) {
				pcie_bdf_t bdf = PCIE_BDF(bus, dev, func);
				uint32_t data;

				data = pcie_conf_read(bdf, PCIE_CONF_ID);
				if (data == PCIE_ID_NONE) {
					continue;
				}

				if (data == id) {
					return bdf;
				}
			}
		}
	}

	return PCIE_BDF_NONE;
}

static int pcie_init(const struct device *dev)
{
	size_t dev_count, found;

	ARG_UNUSED(dev);

	STRUCT_SECTION_COUNT(pcie_dev, &dev_count);
	found = 0;

	for (int b = 0; b <= PCIE_MAX_BUS; b++) {
		for (int d = 0; d <= PCIE_MAX_DEV; d++) {
			for (int f = 0; f <= PCIE_MAX_FUNC; f++) {
				pcie_bdf_t bdf = PCIE_BDF(b, d, f);
				uint32_t id;

				id = pcie_conf_read(bdf, PCIE_CONF_ID);
				if (id == PCIE_ID_NONE) {
					continue;
				}

				STRUCT_SECTION_FOREACH(pcie_dev, dev) {
					if (dev->bdf != PCIE_BDF_NONE) {
						continue;
					}

					if (dev->id == id) {
						dev->bdf = bdf;
						found++;
						break;
					}
				}

				if (found == dev_count) {
					goto done;
				}
			}
		}
	}

done:
	return 0;
}

SYS_INIT(pcie_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
