/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Generic devicetree interrupt specifier -> Linux virq resolver.
 *
 * Public declaration for the NI IRQ resolver implemented in
 * drivers/misc/niirqresolver.c.
 */

#ifndef _LINUX_NIIRQRESOLVER_H_
#define _LINUX_NIIRQRESOLVER_H_

/**
 * ni_of_resolve_irq - resolve a Linux virq from a devicetree interrupt specifier
 * @compatible: devicetree "compatible" string of the interrupt controller node
 * @args: raw interrupt specifier cells, controller-specific
 * @args_count: number of valid entries in @args (must not exceed MAX_PHANDLE_ARGS)
 *
 * Return: the Linux virtual IRQ number, or 0 if it could not be resolved.
 */
unsigned int ni_of_resolve_irq(const char *compatible, const unsigned int *args,
			       unsigned int args_count);

#endif /* _LINUX_NIIRQRESOLVER_H_ */
