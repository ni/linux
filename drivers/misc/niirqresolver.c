// SPDX-License-Identifier: GPL-2.0
/*
 * Generic devicetree interrupt specifier -> Linux virq resolver.
 *
 * This file has no knowledge of any specific device, board, or interrupt
 * controller: the caller supplies the controller's devicetree "compatible"
 * string and its raw, controller-specific interrupt specifier cells (for
 * example <type, spi-number, trigger-flags> for an ARM GIC).
 */

#include <linux/export.h>
#include <linux/niirqresolver.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/types.h>

/**
 * ni_of_resolve_irq - resolve a Linux virq from a devicetree interrupt specifier
 * @compatible: devicetree "compatible" string of the interrupt controller node
 * @args: raw interrupt specifier cells, controller-specific
 * @args_count: number of valid entries in @args (must not exceed MAX_PHANDLE_ARGS)
 *
 * Finds the first devicetree node matching @compatible and asks its
 * irqdomain to map @args to a Linux virq. Repeated calls with the same
 * arguments return the same virq.
 *
 * Return: the Linux virtual IRQ number, or 0 if it could not be resolved
 * (no matching node, args_count out of range, or the domain couldn't map it).
 */
unsigned int ni_of_resolve_irq(const char *compatible, const unsigned int *args,
				unsigned int args_count)
{
	struct device_node *node;
	struct of_phandle_args irq_spec;
	unsigned int virq;
	unsigned int i;

	if (args_count > MAX_PHANDLE_ARGS)
		return 0;

	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node)
		return 0;

	irq_spec.np = node;
	irq_spec.args_count = args_count;
	for (i = 0; i < args_count; i++)
		irq_spec.args[i] = args[i];

	virq = irq_create_of_mapping(&irq_spec);

	of_node_put(node);

	return virq;
}
EXPORT_SYMBOL(ni_of_resolve_irq);
