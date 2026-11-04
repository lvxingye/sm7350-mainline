// SPDX-License-Identifier: GPL-2.0-only
/*
 * renoir_battery.c - simple voltage-based battery monitor for Xiaomi renoir
 *
 * The stock/Android charger stack obtains real state-of-charge from the ADSP
 * charger firmware over PMIC GLINK.  Mainline has no ADSP charger path yet,
 * but PMK8350's VADC exposes PM8350B_ADC7_VBAT_SNS, which is enough to give
 * the Linux power_supply class a coarse battery percentage.
 *
 * This is intentionally a voltage/OCV estimate, not a coulomb counter.  It
 * provides present/capacity/voltage so UPower and desktop shells can show a
 * battery; it does not claim charger status or accurate runtime prediction.
 */

#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define RENOIR_BATTERY_POLL_MS		10000
#define RENOIR_BATTERY_VOLT_MAX_UV	4400000
#define RENOIR_BATTERY_VOLT_MIN_UV	3400000
#define RENOIR_BATTERY_DESIGN_UAH	4250000
#define RENOIR_BATTERY_DESIGN_UWH	16400000

struct renoir_battery {
	struct device *dev;
	struct iio_channel *vbat;
	struct power_supply *psy;
	struct delayed_work poll_work;
	struct mutex lock;
	int voltage_uv;
	int capacity;
	bool present;
};

/* Coarse single-cell Li-ion OCV curve, high -> low. */
static const struct {
	int uv;
	int pct;
} renoir_ocv_curve[] = {
	{ 4400000, 100 },
	{ 4300000,  95 },
	{ 4200000,  87 },
	{ 4100000,  76 },
	{ 4000000,  64 },
	{ 3900000,  50 },
	{ 3800000,  36 },
	{ 3700000,  23 },
	{ 3600000,  12 },
	{ 3500000,   5 },
	{ 3400000,   0 },
};

static int renoir_voltage_to_capacity(int uv)
{
	int i;

	if (uv >= renoir_ocv_curve[0].uv)
		return 100;
	if (uv <= renoir_ocv_curve[ARRAY_SIZE(renoir_ocv_curve) - 1].uv)
		return 0;

	for (i = 1; i < ARRAY_SIZE(renoir_ocv_curve); i++) {
		if (uv >= renoir_ocv_curve[i].uv) {
			int hi_uv = renoir_ocv_curve[i - 1].uv;
			int lo_uv = renoir_ocv_curve[i].uv;
			int hi_pct = renoir_ocv_curve[i - 1].pct;
			int lo_pct = renoir_ocv_curve[i].pct;

			return lo_pct + (uv - lo_uv) *
			       (hi_pct - lo_pct) / (hi_uv - lo_uv);
		}
	}

	return 0;
}

static void renoir_battery_poll(struct work_struct *work)
{
	struct renoir_battery *bat =
		container_of(to_delayed_work(work), struct renoir_battery,
			     poll_work);
	int val, ret;

	ret = iio_read_channel_processed(bat->vbat, &val);
	if (ret < 0) {
		dev_warn_ratelimited(bat->dev,
				     "failed to read VBAT ADC: %d\n", ret);
		goto out;
	}

	/* qcom-vadc processed voltage is normally mV; tolerate uV too. */
	mutex_lock(&bat->lock);
	bat->voltage_uv = val > 10000 ? val : val * 1000;
	bat->capacity = renoir_voltage_to_capacity(bat->voltage_uv);
	bat->present = true;
	mutex_unlock(&bat->lock);

	power_supply_changed(bat->psy);

out:
	schedule_delayed_work(&bat->poll_work,
			      msecs_to_jiffies(RENOIR_BATTERY_POLL_MS));
}

static int renoir_battery_get_property(struct power_supply *psy,
				       enum power_supply_property prop,
				       union power_supply_propval *val)
{
	struct renoir_battery *bat = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&bat->lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bat->present;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		/* No charger/CC driver yet: do not pretend to know. */
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = bat->capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (bat->capacity >= 95)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (bat->capacity >= 60)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		else if (bat->capacity >= 20)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		else if (bat->capacity >= 5)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bat->voltage_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = RENOIR_BATTERY_VOLT_MAX_UV;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = RENOIR_BATTERY_VOLT_MIN_UV;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = RENOIR_BATTERY_DESIGN_UAH;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = RENOIR_BATTERY_DESIGN_UAH;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = (bat->capacity * RENOIR_BATTERY_DESIGN_UAH) / 100;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = RENOIR_BATTERY_DESIGN_UWH;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = RENOIR_BATTERY_DESIGN_UWH;
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		val->intval = (bat->capacity * RENOIR_BATTERY_DESIGN_UWH) / 100;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Renoir battery";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Xiaomi";
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		ret = -ENODATA;
		break;
	}
	mutex_unlock(&bat->lock);

	return ret;
}

static enum power_supply_property renoir_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SCOPE,
};

static const struct power_supply_desc renoir_battery_desc = {
	.name			= "battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= renoir_battery_props,
	.num_properties		= ARRAY_SIZE(renoir_battery_props),
	.get_property		= renoir_battery_get_property,
};

static int renoir_battery_probe(struct platform_device *pdev)
{
	struct renoir_battery *bat;
	struct power_supply_config cfg = {};

	bat = devm_kzalloc(&pdev->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = &pdev->dev;
	mutex_init(&bat->lock);
	bat->voltage_uv = RENOIR_BATTERY_VOLT_MIN_UV;
	bat->capacity = 0;
	bat->present = true;

	bat->vbat = devm_iio_channel_get(&pdev->dev, "voltage");
	if (IS_ERR(bat->vbat))
		return dev_err_probe(&pdev->dev, PTR_ERR(bat->vbat),
				     "failed to get VBAT IIO channel\n");

	cfg.fwnode = dev_fwnode(&pdev->dev);
	cfg.drv_data = bat;

	bat->psy = devm_power_supply_register(&pdev->dev,
					      &renoir_battery_desc, &cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(bat->psy),
				     "failed to register power supply\n");

	platform_set_drvdata(pdev, bat);

	INIT_DELAYED_WORK(&bat->poll_work, renoir_battery_poll);
	schedule_delayed_work(&bat->poll_work, 0);

	dev_info(&pdev->dev, "voltage-based battery monitor registered\n");
	return 0;
}

static void renoir_battery_remove(struct platform_device *pdev)
{
	struct renoir_battery *bat = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&bat->poll_work);
}

static const struct of_device_id renoir_battery_of_match[] = {
	{ .compatible = "xiaomi,renoir-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, renoir_battery_of_match);

static struct platform_driver renoir_battery_driver = {
	.driver = {
		.name = "renoir-battery",
		.of_match_table = renoir_battery_of_match,
	},
	.probe = renoir_battery_probe,
	.remove = renoir_battery_remove,
};
module_platform_driver(renoir_battery_driver);

MODULE_DESCRIPTION("Xiaomi renoir voltage-based battery monitor");
MODULE_LICENSE("GPL");
