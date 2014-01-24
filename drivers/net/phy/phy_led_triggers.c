/* Copyright (C) 2012 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/leds.h>
#include <linux/phy.h>

void phy_led_trigger_change_speed(struct phy_device *phy)
{
	struct phy_led_trigger *plt;

	if (!phy->link) {
		if (phy->last_triggered) {
			led_trigger_event(&phy->last_triggered->trigger,
					  LED_OFF);
			phy->last_triggered = NULL;
			return;
		}
		return;
	}

	switch (phy->speed) {
	case SPEED_10:
		plt = &phy->phy_led_trigger[0];
		break;
	case SPEED_100:
		plt = &phy->phy_led_trigger[1];
		break;
	case SPEED_1000:
		plt = &phy->phy_led_trigger[2];
		break;
	case SPEED_2500:
		plt = &phy->phy_led_trigger[3];
		break;
	case SPEED_10000:
		plt = &phy->phy_led_trigger[4];
		break;
	default:
		plt = NULL;
		break;
	}

	if (plt != phy->last_triggered) {
		led_trigger_event(&phy->last_triggered->trigger, LED_OFF);
		led_trigger_event(&plt->trigger, LED_FULL);
		phy->last_triggered = plt;
	}
}

static int phy_led_trigger_register(struct phy_device *phy,
				    struct phy_led_trigger *plt, int i)
{
	static const char * const name_suffix[] = {
		"10Mb",
		"100Mb",
		"Gb",
		"2.5Gb",
		"10GbE",
	};
	snprintf(plt->name, sizeof(plt->name), PHY_ID_FMT ":%s", phy->mdio.bus->id,
			phy->mdio.addr, name_suffix[i]);
	plt->trigger.name = plt->name;
	return led_trigger_register(&plt->trigger);
}

static void phy_led_trigger_unregister(struct phy_led_trigger *plt)
{
	led_trigger_unregister(&plt->trigger);
}

int phy_led_triggers_register(struct phy_device *phy)
{
	int i, err;
	for (i = 0; i < ARRAY_SIZE(phy->phy_led_trigger); i++) {
		err = phy_led_trigger_register(phy, &phy->phy_led_trigger[i],
					       i);
		if (err)
			goto out_unreg;
	}

	phy->last_triggered = NULL;
	phy_led_trigger_change_speed(phy);

	return 0;

out_unreg:
	while (i--)
		phy_led_trigger_unregister(&phy->phy_led_trigger[i]);
	return err;
}

void phy_led_triggers_unregister(struct phy_device *phy)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(phy->phy_led_trigger); i++)
		phy_led_trigger_unregister(&phy->phy_led_trigger[i]);
}
