/*
* Copyright (C) 2019 Samsung Electronics Co.Ltd
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <linux/mfd/samsung/s2mu107.h>
//#include <linux/muic/s2mu107-muic.h>
//#include <linux/ccic/usbpd-s2mu107.h>
#include "s2mu107_switching_charger.h"
//#include "include/s2mu107_pmeter.h"
#if defined(CONFIG_LEDS_S2MU107_FLASH)
#include <linux/leds-s2mu107.h>
#endif
#ifdef CONFIG_USB_HOST_NOTIFY
#include <linux/usb_notify.h>
#endif

#define ENABLE 1
#define DISABLE 0

#define FIRST_TOPOFF 1
#define SECOND_TOPOFF 2

#if defined(CONFIG_SEC_FACTORY)
extern int factory_mode;
#endif

static char *s2mu107_supplied_to[] = {
	"battery",
};

static enum power_supply_property s2mu107_sw_charger_props[] = {
};

static enum power_supply_property s2mu107_otg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int s2mu107_sc_get_charging_health(struct s2mu107_sc_data *sw_charger);
static void s2mu107_sc_set_input_current_limit(struct s2mu107_sc_data *sw_charger, int charging_current);
static int s2mu107_sc_get_input_current_limit(struct s2mu107_sc_data *sw_charger);
static void s2mu107_sc_bat_state(struct s2mu107_sc_data *sw_charger, int onoff);

static void s2mu107_sc_test_read(struct i2c_client *i2c)
{
	u8 data;
	char str[1016] = {0,};
	int i;

	/* INT_MASK, SC_STATUS, SC_CTRL */
	for (i = 0x0A; i <= 0x2D; i++) {
		s2mu107_read_reg(i2c, i, &data);

		sprintf(str+strlen(str), "0x%02x:0x%02x, ", i, data);
	}

	pr_err("%s : %s\n", __func__, str);
}

static int s2mu107_sc_otg_control(
		struct s2mu107_sc_data *sw_charger, bool enable)
{
	u8 sc_sts4, sc_ctrl0, temp;
	pr_info("%s: called charger otg control : %s\n", __func__,
			enable ? "ON" : "OFF");

	if (sw_charger->is_charging) {
		pr_info("%s: OTG notification is received with switching charger on!!!\n", __func__);
		pr_info("%s: is_charging: %d, otg_on: %d",
				__func__, sw_charger->is_charging, sw_charger->otg_on);
		s2mu107_sc_test_read(sw_charger->i2c);
		return 0;
	}

	if (sw_charger->otg_on == enable)
		return 0;

	mutex_lock(&sw_charger->charger_mutex);
	if (!enable) {
		s2mu107_update_reg(sw_charger->i2c,
				S2MU107_SC_CTRL0, CHG_MODE, REG_MODE_MASK);
		pr_info("%s: OTG off, boost w/a\n", __func__);
		/* prevent boost malfunction */
		if (sw_charger->rev_id == EVT1)
			s2mu107_sc_bat_state(sw_charger, 0);
	} else {
		s2mu107_read_reg(sw_charger->i2c, 0x77, &temp);
		if ((sw_charger->rev_id == EVT1) && ((temp & 0x80) == 0x80))
			s2mu107_sc_bat_state(sw_charger, 0);
		s2mu107_update_reg(sw_charger->i2c,
				S2MU107_SC_CTRL3,
				S2MU107_SET_OTG_OCP_1200mA << SET_OTG_OCP_SHIFT,
				SET_OTG_OCP_MASK);

		pr_info("%s: OTG on, boost w/a\n", __func__);

		/* WCIN PTR ON : 0x5F[5] = 1 */
		s2mu107_update_reg(sw_charger->i2c, S2MU107_DC_TEST4, 0x20, 0x20);
		s2mu107_update_reg(sw_charger->i2c,
				S2MU107_SC_CTRL0, OTG_BST_MODE, REG_MODE_MASK);

		sw_charger->cable_type = POWER_SUPPLY_TYPE_OTG;

		/* prevent boost malfunction */
		if (sw_charger->rev_id == EVT1)
			s2mu107_sc_bat_state(sw_charger, 1);
	}

	sw_charger->otg_on = enable;
	mutex_unlock(&sw_charger->charger_mutex);

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS4, &sc_sts4);
	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL0, &sc_ctrl0);
	pr_info("%s S2MU107_SC_STATUS4: 0x%x\n", __func__, sc_sts4);
	pr_info("%s S2MU107_SC_CTRL0: 0x%x\n", __func__, sc_ctrl0);

	power_supply_changed(sw_charger->psy_otg);
	return enable;
}

static void s2mu107_sc_onoff(
	struct s2mu107_sc_data *sw_charger, int onoff)
{
	u8 val;
	u8 buck_mode = BUCK_MODE;

#if defined(CONFIG_SEC_FACTORY)
	if (factory_mode) {
		pr_info("%s: Factory Mode Skip CHG_EN Control\n", __func__);
		return;
	}
#endif

	if (sw_charger->otg_on) {
		pr_info("[DEBUG] %s: skipped set(%d) : OTG is on\n", __func__, onoff);
		return;
	}

	if (onoff > 0) {
		pr_info("[DEBUG]%s: turn on switching charger\n", __func__);
		/* WCIN PTR ON : 0x5F[5] = 1 */
		s2mu107_update_reg(sw_charger->i2c, S2MU107_DC_TEST4, 0x20, 0x20);
		s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, CHG_MODE, REG_MODE_MASK);

		/* timer fault set 16hr(max) */
		s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL13,
				S2MU107_FC_CHG_TIMER_16hr << SET_TIME_FC_CHG_SHIFT,
				SET_TIME_FC_CHG_MASK);
	} else {
		s2mu107_read_reg(sw_charger->i2c, 0x41, &val);
		if ((val & 0x01) == 0x01)
			buck_mode = 0x0D;

		pr_info("[DEBUG] %s: turn off switching charger\n", __func__);
		s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, buck_mode, REG_MODE_MASK);
	}
}

static void s2mu107_sc_set_buck(
	struct s2mu107_sc_data *sw_charger, int enable) {

	if (enable) {
		pr_info("[DEBUG]%s: set buck on\n", __func__);
		s2mu107_sc_onoff(sw_charger, sw_charger->is_charging);
	} else {
		pr_info("[DEBUG]%s: set buck off (charger off mode)\n", __func__);
		/* TODO : do not buck off! */
		/* need to consider direct charger */
		s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, CHARGER_OFF_MODE, REG_MODE_MASK);
	}
}

static void s2mu107_sc_set_regulation_vsys(
	struct s2mu107_sc_data *sw_charger, int vsys)
{
	u8 data;

	pr_info("[DEBUG]%s: VSYS regulation %d\n", __func__, vsys);
	if (vsys <= 3600)
		data = 0;
	else if (vsys > 3600 && vsys <= 4800)
		data = (vsys - 3600) / 100;
	else
		data = 0x0C;

	s2mu107_update_reg(sw_charger->i2c,
		S2MU107_SC_CTRL8, data << SET_VSYS_SHIFT, SET_VSYS_MASK);
}

static void s2mu107_sc_set_regulation_voltage(
		struct s2mu107_sc_data *sw_charger, int float_voltage)
{
	u8 data;

#if defined(CONFIG_SEC_FACTORY)
	if (factory_mode)
		return;
#endif

	pr_info("[DEBUG]%s: float_voltage %d\n", __func__, float_voltage);
	if (float_voltage <= 3900)
		data = 0;
	else if (float_voltage > 3900 && float_voltage <= 4535)
		data = (float_voltage - 3900) / 5;
	else
		data = 0x7F;

	s2mu107_update_reg(sw_charger->i2c,
			S2MU107_SC_CTRL5, data << SET_VF_VBAT_SHIFT, SET_VF_VBAT_MASK);
}

static int s2mu107_sc_get_regulation_voltage(struct s2mu107_sc_data *sw_charger)
{
	u8 reg_data = 0;
	int float_voltage;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL5, &reg_data);
	reg_data &= 0x7F;
	float_voltage = reg_data * 5 + 3900;
	pr_debug("%s: battery cv reg : 0x%x, float voltage val : %d\n",
			__func__, reg_data, float_voltage);

	return float_voltage;
}

static void s2mu107_sc_set_input_current_limit(
		struct s2mu107_sc_data *sw_charger, int charging_current)
{
	u8 data;

#if defined(CONFIG_SEC_FACTORY)
	if (factory_mode)
		return;
#endif

	if (charging_current <= 50)
		data = 0x00;
	else if (charging_current <= 100)
		data = 0x02;
	else if (charging_current > 100 && charging_current <= 3000)
		data = (charging_current - 50) / 25;
	else
		data = 0x62;

	s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL1,
			data << INPUT_CURRENT_LIMIT_SHIFT, INPUT_CURRENT_LIMIT_MASK);

	pr_info("[DEBUG]%s: current  %d, 0x%x\n", __func__, charging_current, data);

#if EN_TEST_READ
	s2mu107_sc_test_read(sw_charger->i2c);
#endif
}

static int s2mu107_sc_get_input_current_limit(struct s2mu107_sc_data *sw_charger)
{
	u8 data;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL1, &data);
	if (data < 0)
		return data;

	data = data & INPUT_CURRENT_LIMIT_MASK;
	if (data > 0x76) {
		pr_err("%s: Invalid current limit in register\n", __func__);
		data = 0x76;
	}
	return  data * 25 + 50;
}

static void s2mu107_sc_set_fast_charging_current(
		struct s2mu107_sc_data *sw_charger, int charging_current)
{
	u8 data;

#if defined(CONFIG_SEC_FACTORY)
	if (factory_mode)
		return;
#endif

	if (charging_current <= 100)
		data = 0x01;
	else if (charging_current > 100 && charging_current <= 3200)
		data = (charging_current / 50) - 1;
	else
		data = 0x3D;

	s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL7,
			data << FAST_CHARGING_CURRENT_SHIFT, FAST_CHARGING_CURRENT_MASK);

	pr_info("[DEBUG]%s: current  %d, 0x%02x\n", __func__, charging_current, data);

#if EN_TEST_READ
	s2mu107_sc_test_read(sw_charger->i2c);
#endif
}

static int s2mu107_sc_get_fast_charging_current(
		struct s2mu107_sc_data *sw_charger)
{
	u8 data;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL7, &data);
	if (data < 0)
		return data;

	data = data & FAST_CHARGING_CURRENT_MASK;

	if (data > 0x3F) {
		pr_err("%s: Invalid fast charging current in register\n", __func__);
		data = 0x3F;
	}

	if ((data + 1) * 50 == 50)
		return 100;
	else
		return ((data + 1) * 50);
}

static void s2mu107_sc_set_topoff_current(
		struct s2mu107_sc_data *sw_charger,
		int eoc_1st_2nd, int current_limit)
{
	int data;
	union power_supply_propval value;
	struct power_supply *psy;

	pr_info("[DEBUG]%s: current  %d\n", __func__, current_limit);
	if (current_limit <= 100)
		data = 0;
	else if (current_limit > 100 && current_limit <= 875)
		data = (current_limit - 100) / 25;
	else
		data = 0x1F;

	switch (eoc_1st_2nd) {
	case FIRST_TOPOFF:
		s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL15,
				data << FIRST_TOPOFF_CURRENT_SHIFT, FIRST_TOPOFF_CURRENT_MASK);
		psy = power_supply_get_by_name(sw_charger->pdata->fuelgauge_name);
		if (!psy)
			pr_err("%s, fail to set topoff current to FG\n", __func__);
		else {
			value.intval = current_limit;
			power_supply_set_property(psy, POWER_SUPPLY_PROP_CURRENT_FULL, &value);
		}
		break;
	case SECOND_TOPOFF:
		s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL16,
				data << SECOND_TOPOFF_CURRENT_SHIFT, SECOND_TOPOFF_CURRENT_MASK);
		break;
	default:
		break;
	}
}

static int s2mu107_sc_get_topoff_current(
		struct s2mu107_sc_data *sw_charger)
{
	u8 data;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL15, &data);
	if (data < 0)
		return data;

	data = data & FIRST_TOPOFF_CURRENT_MASK;

	if (data > 0x1F)
		data = 0x1F;
	return data * 25 + 100;
}

static bool s2mu107_sc_init(struct s2mu107_sc_data *sw_charger)
{
	u8 temp;

	s2mu107_update_reg(sw_charger->i2c, 0x90, 0x00, 0x04);

	/* To prevent entering watchdog issue case we set WDT_CLR not to clear before enabling WDT */
	s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL12, 0x00, WDT_CLR_MASK);

	/* set watchdog timer to 80 seconds */
	s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL12,
			S2MU107_WDT_TIMER_80s << WDT_TIME_SHIFT,
			WDT_TIME_MASK);

	/* enable watchdog timer , turn of charger only in case of watchdog */
	s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL12,
			ENABLE << SET_EN_WDT_SHIFT | DISABLE << SET_EN_WDT_AP_RESET_SHIFT,
			SET_EN_WDT_MASK | SET_EN_WDT_AP_RESET_MASK);

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL12, &temp);
	pr_info("%s : for WDT setting S2MU107_SC_CTRL12 : 0x%x\n", __func__, temp);
#if 0
	/* Type-C reset off */
	s2mu107_update_reg(charger->i2c, 0xEC, 0x00, 0x80);

	/* MRSTB 1s set */
	s2mu107_write_reg(charger->i2c, 0xE5, 0x08);

	/* OTG Fault debounce time set 15ms */
	s2mu107_update_reg(charger->i2c, 0x94, 0x0C, 0x0C);
#endif
#if 0 	//ndef CONFIG_SEC_FACTORY	// temp for bringup
	/* Prevent sudden power off when water detect */
	if (!factory_mode) {
		pr_info ("%s Normal booting\n", __func__);
		/*
		s2mu107_update_reg(charger->i2c, 0x88, 0x20, 0x20);
		s2mu107_write_reg(charger->i2c, 0xF3, 0x00);
		s2mu107_update_reg(charger->i2c, 0x8C, 0x00, 0x80);
		s2mu107_update_reg(charger->i2c, 0x90, 0x00, 0x04);
		*/
	}
#endif

	/* input TR open */
	s2mu107_write_reg(sw_charger->i2c, S2MU107_DC_TEST4, 0x70);

	/* QBAT on when 523k is removed */
#if defined(CONFIG_SEC_FACTORY)
	if (factory_mode)
		s2mu107_write_reg(sw_charger->i2c_common, 0xF1, 0x02);
#endif

	/* w/a for plug-off */
	s2mu107_update_reg(sw_charger->i2c, 0x81, 0x20, 0xE0);

	/* protect buck operation, buck NTR off */
	s2mu107_write_reg(sw_charger->i2c, 0x3A, 0x57);

	return true;
}

static int s2mu107_sc_get_charging_status(
		struct s2mu107_sc_data *sw_charger)
{
	int status = POWER_SUPPLY_STATUS_UNKNOWN;
	int ret;
	u8 sc_sts0, sc_sts1;
	union power_supply_propval value;
	struct power_supply *psy;

	ret = s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS0, &sc_sts0);
	ret = s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS1, &sc_sts1);

	psy = power_supply_get_by_name(sw_charger->pdata->fuelgauge_name);
	if (!psy)
		return -EINVAL;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_AVG, &value);
	if (ret < 0)
		pr_err("%s: Fail to execute property\n", __func__);

	if (ret < 0)
		return status;

	if (sc_sts1 & 0x80)
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (sc_sts1 & 0x02 || sc_sts1 & 0x01) {
		pr_info("%s: full check curr_avg(%d), topoff_curr(%d)\n",
				__func__, value.intval, sw_charger->topoff_current);
		if (value.intval < sw_charger->topoff_current)
			status = POWER_SUPPLY_STATUS_FULL;
		else
			status = POWER_SUPPLY_STATUS_CHARGING;
	} else if ((sc_sts0 & 0xE0) == 0xA0 || (sc_sts0 & 0xE0) == 0x60)
		status = POWER_SUPPLY_STATUS_CHARGING;
	else
	status = POWER_SUPPLY_STATUS_NOT_CHARGING;

#if EN_TEST_READ
	s2mu107_sc_test_read(sw_charger->i2c);
#endif
	return status;
}

static int s2mu107_sc_get_charge_type(struct s2mu107_sc_data *sw_charger)
{
	int status = POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	u8 ret;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS5, &ret);
	if (ret < 0)
		pr_err("%s fail\n", __func__);

	switch ((ret & BAT_STATUS_MASK) >> BAT_STATUS_SHIFT) {
	case 0x6:
		status = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case 0x5:
		status = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	}
	return status;
}
#if 0
static bool s2mu107_sc_get_batt_present(struct s2mu107_sc_data *sw_charger)
{
	u8 ret;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS5, &ret);
	if (ret < 0)
		return false;

	return (ret & BATID_STATUS_MASK) ? true : false;
}
#endif

static void s2mu107_sc_wdt_clear(struct s2mu107_sc_data *sw_charger)
{
	u8 reg_data, chg_fault_status;

	/* watchdog kick */
	s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL12,
			0x1 << WDT_CLR_SHIFT, WDT_CLR_MASK);

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS1, &reg_data);
	chg_fault_status = (reg_data & CHG_FAULT_STATUS_MASK) >> CHG_FAULT_STATUS_SHIFT;

	if ((chg_fault_status == CHG_STATUS_WD_SUSPEND) ||
			(chg_fault_status == CHG_STATUS_WD_RST)) {
		pr_info("%s: watchdog error status(0x%02x,%d)\n",
				__func__, reg_data, chg_fault_status);
		if (sw_charger->is_charging) {
			pr_info("%s: toggle charger\n", __func__);
			s2mu107_sc_onoff(sw_charger, false);
			s2mu107_sc_onoff(sw_charger, true);
		}
	}
}

static int s2mu107_sc_get_charging_health(struct s2mu107_sc_data *sw_charger)
{

	u8 ret;
	union power_supply_propval value;
	struct power_supply *psy;

	if (sw_charger->is_charging)
		s2mu107_sc_wdt_clear(sw_charger);

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS0, &ret);
	pr_info("[DEBUG] %s: S2MU107_CHG_STATUS0 0x%x\n", __func__, ret);
	if (ret < 0)
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	ret = (ret & (CHGIN_STATUS_MASK)) >> CHGIN_STATUS_SHIFT;

	switch (ret) {
	case 0x03:
	case 0x05:
		sw_charger->ovp = false;
		sw_charger->unhealth_cnt = 0;
		return POWER_SUPPLY_HEALTH_GOOD;
	default:
		break;
	}

	sw_charger->unhealth_cnt++;
	if (sw_charger->unhealth_cnt < HEALTH_DEBOUNCE_CNT)
		return POWER_SUPPLY_HEALTH_GOOD;

	/* 005 need to check ovp & health count */
	sw_charger->unhealth_cnt = HEALTH_DEBOUNCE_CNT;
	if (sw_charger->ovp)
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;

	psy = power_supply_get_by_name("battery");
	if (!psy)
		return -EINVAL;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
	if (ret < 0)
		pr_err("%s: Fail to execute property\n", __func__);

	if (value.intval == SEC_BATTERY_CABLE_PDIC)
		return POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	else
		return POWER_SUPPLY_HEALTH_GOOD;

#if EN_TEST_READ
	s2mu107_sc_test_read(sw_charger->i2c);
#endif
}

static int s2mu107_sc_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int chg_curr, aicr;
	struct s2mu107_sc_data *sw_charger =
		power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property)psp;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = sw_charger->is_charging ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = s2mu107_sc_get_charging_status(sw_charger);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = s2mu107_sc_get_charging_health(sw_charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = s2mu107_sc_get_input_current_limit(sw_charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (sw_charger->charging_current) {
			aicr = s2mu107_sc_get_input_current_limit(sw_charger);
			chg_curr = s2mu107_sc_get_fast_charging_current(sw_charger);
			val->intval = MINVAL(aicr, chg_curr);
		} else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = s2mu107_sc_get_fast_charging_current(sw_charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_FULL:
		val->intval = s2mu107_sc_get_topoff_current(sw_charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = s2mu107_sc_get_charge_type(sw_charger);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = s2mu107_sc_get_regulation_voltage(sw_charger);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		//val->intval = s2mu107_get_batt_present(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		val->intval = sw_charger->is_charging;
		break;
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
			case POWER_SUPPLY_EXT_PROP_MONITOR_WORK:
				s2mu107_sc_test_read(sw_charger->i2c);
				break;
			default:
				return -EINVAL;
		}
	default:
		return -EINVAL;
	}
	return 0;
}

static int s2mu107_sc_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct s2mu107_sc_data *sw_charger = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property)psp;
	int buck_state = ENABLE;
	union power_supply_propval value;
	int ret;
	//u8 data = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		sw_charger->status = val->intval;
		break;
		/* val->intval : type */
	case POWER_SUPPLY_PROP_ONLINE:
		sw_charger->cable_type = val->intval;
		if (sw_charger->cable_type != POWER_SUPPLY_TYPE_OTG) {
			if (sw_charger->cable_type == POWER_SUPPLY_TYPE_BATTERY ||
			sw_charger->cable_type == POWER_SUPPLY_TYPE_UNKNOWN) {
				pr_err("[DEBUG]%s:[BATT] Type Battery\n", __func__);
				value.intval = 0;
			} else {
				value.intval = 1;
			}

			psy = power_supply_get_by_name(sw_charger->pdata->fuelgauge_name);
			if (!psy)
				return -EINVAL;
			ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_ENERGY_AVG, &value);
			if (ret < 0)
				pr_err("%s: Fail to execute property\n", __func__);
		} else {
			pr_info("[DEBUG]%s:Cable Type OTG \n", __func__);
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		{
			int input_current = val->intval;

			s2mu107_sc_set_input_current_limit(sw_charger, input_current);
			sw_charger->input_current = input_current;
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		pr_info("[DEBUG] %s: is_charging %d\n", __func__, sw_charger->is_charging);
		sw_charger->charging_current = val->intval;
		/* set charging current */
		s2mu107_sc_set_fast_charging_current(sw_charger, sw_charger->charging_current);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		break;
	case POWER_SUPPLY_PROP_CURRENT_FULL:
		sw_charger->topoff_current = val->intval;
		if (sw_charger->pdata->chg_eoc_dualpath) {
			s2mu107_sc_set_topoff_current(sw_charger, 1, val->intval);
			s2mu107_sc_set_topoff_current(sw_charger, 2, 100);
		} else
			s2mu107_sc_set_topoff_current(sw_charger, 1, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		pr_info("[DEBUG]%s: float voltage(%d)\n", __func__, val->intval);
		sw_charger->pdata->chg_float_voltage = val->intval;
		s2mu107_sc_set_regulation_voltage(sw_charger,
				sw_charger->pdata->chg_float_voltage);
		break;
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
		s2mu107_sc_otg_control(sw_charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		sw_charger->charge_mode = val->intval;

		psy = power_supply_get_by_name("battery");
		if (!psy)
			return -EINVAL;
		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
		if (ret < 0)
			pr_err("%s: Fail to execute property\n", __func__);

		if (value.intval != POWER_SUPPLY_TYPE_OTG) {
			switch (sw_charger->charge_mode) {
			case SEC_BAT_CHG_MODE_BUCK_OFF:
				buck_state = DISABLE;
			case SEC_BAT_CHG_MODE_CHARGING_OFF:
				sw_charger->is_charging = false;
				break;
			case SEC_BAT_CHG_MODE_CHARGING:
				sw_charger->is_charging = true;
				break;
			}
			value.intval = sw_charger->is_charging;

#if 0
			psy = power_supply_get_by_name(sw_charger->pdata->fuelgauge_name);
			if (!psy)
				return -EINVAL;
			ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_CHARGING_ENABLED, &value);
			if (ret < 0)
				pr_err("%s: Fail to execute property\n", __func__);
#endif
			if (buck_state)
				s2mu107_sc_onoff(sw_charger, sw_charger->is_charging);
			else
				s2mu107_sc_set_buck(sw_charger, buck_state);
		} else {
			pr_info("[DEBUG]%s: SKIP CHARGING CONTROL while OTG(%d)\n",
					__func__, value.intval);
		}
		break;
#ifndef CONFIG_SEC_FACTORY
	case POWER_SUPPLY_PROP_FACTORY_MODE:
	/* TODO */
#if 0
		if (val->intval) {
			pr_info("%s : 523K, 301K, 255K\n", __func__);
			s2mu107_update_reg(charger->i2c, 0x88, 0x00, 0x20);
			s2mu107_write_reg(charger->i2c, 0xF3, 0x06);
			s2mu107_update_reg(charger->i2c, 0x8C, 0x80, 0x80);
			s2mu107_update_reg(charger->i2c, 0x90, 0x04, 0x04);
		} else {
			pr_info("%s : 619K, OPEN\n", __func__);
			s2mu107_update_reg(charger->i2c, 0x88, 0x20, 0x20);
			s2mu107_write_reg(charger->i2c, 0xF3, 0x00);
			s2mu107_update_reg(charger->i2c, 0x8C, 0x00, 0x80);
			s2mu107_update_reg(charger->i2c, 0x90, 0x00, 0x04);
		}
#endif
		break;
#endif
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (val->intval) {
			pr_info("%s: Factory Mode Set with 523K, 301K\n", __func__);
#if defined(CONFIG_LEDS_S2MU107_FLASH)
			/* FLED TA only mode */
			s2mu107_fled_set_operation_mode(1);
#endif
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL1,
				0x4E << INPUT_CURRENT_LIMIT_SHIFT, INPUT_CURRENT_LIMIT_MASK);
			s2mu107_sc_set_regulation_vsys(sw_charger, 4000);
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL8, 0x80, 0x80);
			s2mu107_update_reg(sw_charger->i2c_common, 0xE5, 0x08, 0x0F);

			value.intval = SEC_BAT_INBAT_FGSRC_SWITCHING_OFF;
			psy_do_property("s2mu107-fuelgauge", set,
				POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING, value);

 		} else {
			pr_info("%s: Factory Mode Release with 619K\n", __func__);
#if defined(CONFIG_LEDS_S2MU107_FLASH)
			/* FLED Auto control mode */
			s2mu107_fled_set_operation_mode(0);
#endif
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL18, 0x40, 0xC0);
			s2mu107_update_reg(sw_charger->i2c, 0x96, 0x01, 0x01);
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_TEST7, 0x01, 0x03);

			s2mu107_update_reg(sw_charger->i2c_common, 0xE5, 0x0E, 0x0F);
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL1,
				0x12 << INPUT_CURRENT_LIMIT_SHIFT, INPUT_CURRENT_LIMIT_MASK);
			s2mu107_sc_set_regulation_vsys(sw_charger, 4300);

			value.intval = SEC_BAT_INBAT_FGSRC_SWITCHING_ON;
			psy_do_property("s2mu107-fuelgauge", set,
				POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING, value);
		}
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		if (val->intval) {
			pr_info("%s: Disable VBUS UVLO\n", __func__);
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_TEST8, 0xC0, 0xC0);
			msleep(100);
		}
		break;
	case POWER_SUPPLY_PROP_AUTHENTIC:
		if (val->intval) {
			pr_info("%s: From Factory Mode to Bypass Mode Set\n", __func__);

			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, 0x10, 0x30);
			s2mu107_write_reg(sw_charger->i2c, 0x74, 0x00);
			s2mu107_update_reg(sw_charger->i2c, 0x8D, 0x01, 0x01);
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, 0x30, 0x30);

			s2mu107_update_reg(sw_charger->i2c_common, 0xE5, 0x0F, 0x0F);
			s2mu107_update_reg(sw_charger->i2c_common, 0xEF, 0x00, 0x01);
			s2mu107_update_reg(sw_charger->i2c_common, 0xE4, 0x00, 0x80);
			s2mu107_update_reg(sw_charger->i2c_common, 0xEA, 0x80, 0x80);
			s2mu107_update_reg(sw_charger->i2c, 0x71, 0x00, 0x80);

			psy_do_property("s2mu107-usbpd", set,
					POWER_SUPPLY_PROP_AUTHENTIC, value);

			psy_do_property("s2mu107_pmeter", set,
					POWER_SUPPLY_PROP_PM_FACTORY, value);

			pr_info("%s Bypass Mode Set Complete %d\n", __func__, __LINE__);
		} else {
			pr_info("%s: Bypass Mode Release, set off\n", __func__);
			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, 0x00, 0x0F);
			s2mu107_update_reg(sw_charger->i2c, 0x8D, 0x00, 0x01);
		}
		break;
	case POWER_SUPPLY_PROP_FUELGAUGE_RESET:
#if 0
		s2mu107_read_reg(charger->i2c, 0xE3, &data);
		data |= 0x03 << 6;
		s2mu107_write_reg(charger->i2c, 0xE3, data);
		msleep(1000);
		data &= ~(0x03 << 6);
		s2mu107_write_reg(charger->i2c, 0xE3, data);
		msleep(50);
		pr_info("%s: reset fuelgauge when surge occur!\n", __func__);
#endif
		break;
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_FACTORY_VOLTAGE_REGULATION:
			pr_info("%s: Factory Voltage Regulation (%d)\n", __func__, val->intval);
			s2mu107_sc_set_regulation_vsys(sw_charger, val->intval);

			s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL8,
					1 << EN_JIG_REG_AP_SHIFT, EN_JIG_REG_AP_MASK);
			break;
		case POWER_SUPPLY_EXT_PROP_CURRENT_MEASURE:
			if (val->intval) {
				pr_info("%s: Bypass Mode with Keystring\n", __func__);
				/*
				 * Charger/muic interrupt can occur by entering Bypass mode
				 * Disable all interrupt mask for testing current measure.
				 */

				/* VBUS UVLO disable */
				s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_TEST8, 0xC0, 0xC0);

				/* Enter Bypass mode */
				s2mu107_update_reg(sw_charger->i2c, 0x8D, 0x01, 0x01);
				s2mu107_write_reg(sw_charger->i2c, 0x74, 0x00);
				s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, 0x30, 0x30);
				pr_info("%s : %d\n", __func__, __LINE__);

				msleep(200);

				/* QBAT off to prevent SMPL when cable is detached  */
				s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL18, 0xC0, 0xC0);
				s2mu107_update_reg(sw_charger->i2c, 0x96, 0x00, 0x01);
				s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_TEST7, 0x00, 0x03);

				s2mu107_update_reg(sw_charger->i2c_common, 0xE5, 0x08, 0x0F);

				psy_do_property("s2mu107_pmeter", set,
					POWER_SUPPLY_PROP_PM_FACTORY, value);
			} else {
				pr_info("%s: Bypass Mode with Keystring Release, Set Off\n", __func__);
				s2mu107_update_reg(sw_charger->i2c, S2MU107_SC_CTRL0, 0x00, 0x0F);
				s2mu107_update_reg(sw_charger->i2c, 0x8D, 0x00, 0x01);
			}
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2mu107_otg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct s2mu107_sc_data *sw_charger = power_supply_get_drvdata(psy);
	u8 reg;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = sw_charger->otg_on;
		break;
	case POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL:
		s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS4, &reg);
		pr_info("%s: S2MU107_SC_STATUS4 : 0x%X\n", __func__, reg);
		if ((reg & 0xC0) == 0x80)
			val->intval = 1;
		else
			val->intval = 0;
		s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_CTRL0, &reg);
		pr_info("%s: S2MU107_SC_CTRL0 : 0x%X\n", __func__, reg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int s2mu107_otg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct s2mu107_sc_data *sw_charger =  power_supply_get_drvdata(psy);
	union power_supply_propval value;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		value.intval = val->intval;
		pr_info("%s: OTG %s\n", __func__, value.intval > 0 ? "ON" : "OFF");

		psy = power_supply_get_by_name(sw_charger->pdata->sc_name);
		if (!psy)
			return -EINVAL;
		ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, &value);
		if (ret < 0)
			pr_err("%s: Fail to execute property\n", __func__);

		power_supply_changed(sw_charger->psy_otg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void s2mu107_otg_vbus_work(struct work_struct *work)
{
	struct s2mu107_sc_data *sw_charger = container_of(work,
			struct s2mu107_sc_data,
			otg_vbus_work.work);

	/* TODO : need to check boost voltage */
	s2mu107_write_reg(sw_charger->i2c, S2MU107_SC_CTRL11, 0x16);
}

/* TODO */
#if 0
/* s2mu107 interrupt service routine */
static irqreturn_t s2mu107_sc_det_bat_isr(int irq, void *data)
{
	struct s2mu107_sc_data *sw_charger = data;
	u8 val;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_CHG_STATUS3, &val);
	if ((val & DET_BAT_STATUS_MASK) == 0) {
		s2mu107_enable_charger_switch(charger, 0);
		pr_err("charger-off if battery removed\n");
	}
	return IRQ_HANDLED;
}
#endif

static irqreturn_t s2mu107_sc_done_isr(int irq, void *data)
{
	struct s2mu107_sc_data *sw_charger = data;
	u8 val;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS1, &val);
	pr_info("%s , %02x\n", __func__, val);
	if (val & (DONE_STATUS_MASK)) {
		pr_err("add self chg done\n");
		/* add chg done code here */
	}
	return IRQ_HANDLED;
}

static void s2mu107_sc_bat_state(struct s2mu107_sc_data *sw_charger, int onoff)
{
	if (onoff > 0) {
		/* VBUS Valid */
		s2mu107_write_reg(sw_charger->i2c_muic, 0x8F, 0x00);
		s2mu107_write_reg(sw_charger->i2c, 0x8E, 0x68);
	} else {
		/* BAT only */
		s2mu107_write_reg(sw_charger->i2c_muic, 0x8F, 0x80);
		s2mu107_write_reg(sw_charger->i2c, 0x8E, 0xE8);
		s2mu107_update_reg(sw_charger->i2c, 0x77, 0x00, 0x80);
	}
}

static irqreturn_t s2mu107_sc_chgin_isr(int irq, void *data)
{
	struct s2mu107_sc_data *sw_charger = data;
	u8 val;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS0, &val);
	pr_info("%s : STATUS0 %02x\n", __func__, val);

	val = (val & CHGIN_STATUS_MASK) >> CHGIN_STATUS_SHIFT;

	if (val == 0x1 || val == 0x2 || val == 0x3 || val == 0x5) {
		pr_info("%s : VBUS Valid 0x%x, boost & charging w/a\n", __func__, val);
		/* prevent boost malfunction when enabling it */
		if (sw_charger->rev_id == EVT1)
			s2mu107_sc_bat_state(sw_charger, 1);
	} else if (val == 0x0) {
		pr_info("%s : VBUS Invalid 0x%x, boost & charging w/a\n", __func__, val);
		/* prevent boost malfunction when enabling it */
		if (sw_charger->rev_id == EVT1)
			s2mu107_sc_bat_state(sw_charger, 0);
	}

	if (val != 0x00)
		s2mu107_update_reg(sw_charger->i2c, 0x77, 0x80, 0x80);

	return IRQ_HANDLED;
}

static irqreturn_t s2mu107_sc_restart_isr(int irq, void *data)
{
	struct s2mu107_sc_data *sw_charger = data;
	u8 val;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS1, &val);
	pr_info("%s : STATUS1 %02x\n", __func__, val);
	return IRQ_HANDLED;
}

static irqreturn_t s2mu107_sc_fault_isr(int irq, void *data)
{
	struct s2mu107_sc_data *sw_charger = data;
	union power_supply_propval value;
	u8 val;
	u8 fault;

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS1, &val);
	pr_info("%s , %02x\n", __func__, val);

	fault = (val & CHG_FAULT_STATUS_MASK) >> CHG_FAULT_STATUS_SHIFT;

	if (fault == CHG_STATUS_WD_SUSPEND || fault == CHG_STATUS_WD_RST) {
		value.intval = 1;
		pr_info("%s, reset USBPD\n", __func__);
		psy_do_property("s2mu107-usbpd", set,
					POWER_SUPPLY_PROP_USBPD_RESET, value);
	}

	return IRQ_HANDLED;
}

static irqreturn_t s2mu107_otg_isr(int irq, void *data)
{
	struct s2mu107_sc_data *sw_charger = data;
	u8 val = 0;
#if 0
#ifdef CONFIG_USB_HOST_NOTIFY
	struct otg_notify *o_notify;

	o_notify = get_otg_notify();
#endif
#endif

	s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS4, &val);
	pr_info("%s - 1, 0x%02x\n", __func__, val);
	if ((val & 0xC0) == 0x80) {
		/* Try to read the OTG Status after 50ms. */
		msleep(50);
		s2mu107_read_reg(sw_charger->i2c, S2MU107_SC_STATUS4, &val);
		pr_info("%s - 2, 0x%02x\n", __func__, val);
		if ((val & 0xC0) == 0x80) {
			pr_info("%s: bypass overcurrent limit\n", __func__);
#if 0
#ifdef CONFIG_USB_HOST_NOTIFY
			if (o_notify)
				send_otg_notify(o_notify, NOTIFY_EVENT_OVERCURRENT, 0);
#endif
#endif
		}
	}

	return IRQ_HANDLED;
}

static int s2mu107_sc_parse_dt(struct device *dev,
		struct s2mu107_sc_platform_data *pdata)
{
	struct device_node *np = of_find_node_by_name(NULL, "s2mu107-charger");
	int ret = 0;

	if (!np) {
		pr_err("%s np NULL(s2mu107-charger)\n", __func__);
	} else {
		ret = of_property_read_u32(np, "battery,chg_switching_freq",
				&pdata->chg_switching_freq);
		if (ret < 0)
			pr_info("%s: Charger switching FRQ is Empty\n", __func__);
	}

	np = of_find_node_by_name(NULL, "battery");
	if (!np) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_property_read_string(np,
				"battery,fuelgauge_name",
				(char const **)&pdata->fuelgauge_name);
		if (ret < 0)
			pr_info("%s: Fuel-gauge name is Empty\n", __func__);

		ret = of_property_read_u32(np, "battery,chg_float_voltage",
				&pdata->chg_float_voltage);
		if (ret) {
			pr_info("%s: battery,chg_float_voltage is Empty\n", __func__);
			pdata->chg_float_voltage = 4200;
		}
		pr_info("%s: battery,chg_float_voltage is %d\n",
				__func__, pdata->chg_float_voltage);

		pdata->chg_eoc_dualpath = of_property_read_bool(np,
				"battery,chg_eoc_dualpath");
	}

	np = of_find_node_by_name(NULL, "sec-multi-charger");
	if (!np) {
		pr_err("%s np NULL(sec-multi-charger)\n", __func__);
	} else {
		ret = of_property_read_string(np,
				"charger,main_charger",
				(char const **)&pdata->sc_name);
		if (ret < 0)
			pr_info("%s: Charger name is Empty\n", __func__);
	}
#if 0
		p = of_get_property(np, "battery,input_current_limit", &len);
		if (!p)
			return 1;

		len = len / sizeof(u32);

		pdata->charging_current =
			kzalloc(sizeof(sec_charging_current_t) * len,
					GFP_KERNEL);

		for (i = 0; i < len; i++) {
			ret = of_property_read_u32_index(np,
					"battery,input_current_limit", i,
					&pdata->charging_current[i].input_current_limit);
			if (ret)
				pr_info("%s : Input_current_limit is Empty\n",
						__func__);

			ret = of_property_read_u32_index(np,
					"battery,fast_charging_current", i,
					&pdata->charging_current[i].fast_charging_current);
			if (ret)
				pr_info("%s : Fast charging current is Empty\n",
						__func__);

			ret = of_property_read_u32_index(np,
					"battery,full_check_current", i,
					&pdata->charging_current[i].full_check_current);
			if (ret)
				pr_info("%s : Full check current is Empty\n",
						__func__);
		}
	}
#endif

	pr_info("%s DT file parsed succesfully, %d\n", __func__, ret);
	return ret;
}

/* if need to set s2mu107 pdata */
static const struct of_device_id s2mu107_sw_charger_match_table[] = {
	{ .compatible = "samsung,s2mu107-switching-charger",},
	{},
};

static int s2mu107_switching_charger_probe(struct platform_device *pdev)
{
	struct s2mu107_dev *s2mu107 = dev_get_drvdata(pdev->dev.parent);
	struct s2mu107_platform_data *pdata = dev_get_platdata(s2mu107->dev);
	struct s2mu107_sc_data *sw_charger;
	struct power_supply_config psy_cfg = {};
	int ret = 0;

	pr_info("%s:[BATT] S2MU107 switching charger driver probe\n", __func__);
	sw_charger = kzalloc(sizeof(*sw_charger), GFP_KERNEL);
	if (!sw_charger)
		return -ENOMEM;

	mutex_init(&sw_charger->charger_mutex);
	sw_charger->otg_on = false;

	sw_charger->dev = &pdev->dev;
	sw_charger->i2c = s2mu107->chg;
	sw_charger->i2c_common = s2mu107->i2c;
	sw_charger->i2c_muic = s2mu107->muic;
	sw_charger->rev_id = s2mu107->pmic_ver;

	sw_charger->pdata = devm_kzalloc(&pdev->dev, sizeof(*(sw_charger->pdata)),
			GFP_KERNEL);
	if (!sw_charger->pdata) {
		ret = -ENOMEM;
		goto err_parse_dt_nomem;
	}
	ret = s2mu107_sc_parse_dt(&pdev->dev, sw_charger->pdata);
	if (ret < 0)
		goto err_parse_dt;

	platform_set_drvdata(pdev, sw_charger);

	if (sw_charger->pdata->sc_name == NULL)
		sw_charger->pdata->sc_name = "s2mu107-switching-charger";
	if (sw_charger->pdata->fuelgauge_name == NULL)
		sw_charger->pdata->fuelgauge_name = "s2mu107-fuelgauge";

	sw_charger->psy_sc_desc.name           = sw_charger->pdata->sc_name;
	sw_charger->psy_sc_desc.type           = POWER_SUPPLY_TYPE_UNKNOWN;
	sw_charger->psy_sc_desc.get_property   = s2mu107_sc_get_property;
	sw_charger->psy_sc_desc.set_property   = s2mu107_sc_set_property;
	sw_charger->psy_sc_desc.properties     = s2mu107_sw_charger_props;
	sw_charger->psy_sc_desc.num_properties = ARRAY_SIZE(s2mu107_sw_charger_props);

	sw_charger->psy_otg_desc.name           = "otg";
	sw_charger->psy_otg_desc.type           = POWER_SUPPLY_TYPE_OTG;
	sw_charger->psy_otg_desc.get_property   = s2mu107_otg_get_property;
	sw_charger->psy_otg_desc.set_property   = s2mu107_otg_set_property;
	sw_charger->psy_otg_desc.properties     = s2mu107_otg_props;
	sw_charger->psy_otg_desc.num_properties = ARRAY_SIZE(s2mu107_otg_props);

	s2mu107_sc_init(sw_charger);
	sw_charger->input_current = s2mu107_sc_get_input_current_limit(sw_charger);
	sw_charger->charging_current = s2mu107_sc_get_fast_charging_current(sw_charger);

	psy_cfg.drv_data = sw_charger;
	psy_cfg.supplied_to = s2mu107_supplied_to;
	psy_cfg.num_supplicants = ARRAY_SIZE(s2mu107_supplied_to);

	sw_charger->psy_sc = power_supply_register(&pdev->dev, &sw_charger->psy_sc_desc, &psy_cfg);
	if (IS_ERR(sw_charger->psy_sc)) {
		pr_err("%s: Failed to Register psy_sc\n", __func__);
		ret = PTR_ERR(sw_charger->psy_sc);
		goto err_power_supply_register;
	}

	sw_charger->psy_otg = power_supply_register(&pdev->dev, &sw_charger->psy_otg_desc, &psy_cfg);
	if (IS_ERR(sw_charger->psy_otg)) {
		pr_err("%s: Failed to Register psy_otg\n", __func__);
		ret = PTR_ERR(sw_charger->psy_otg);
		goto err_power_supply_register_otg;
	}

	sw_charger->charger_wqueue = create_singlethread_workqueue("sw-charger-wq");
	if (!sw_charger->charger_wqueue) {
		pr_info("%s: failed to create wq.\n", __func__);
		ret = -ESRCH;
		goto err_create_wq;
	}

	/*
	 * irq request
	 * if you need to add irq , please refer below code.
	 */

	sw_charger->irq_chgin = pdata->irq_base + S2MU107_SC_IRQ1_CHGIN;
	ret = request_threaded_irq(sw_charger->irq_chgin, NULL,
			s2mu107_sc_chgin_isr, 0, "chgin-irq", sw_charger);
	if (ret < 0) {
		dev_err(s2mu107->dev, "%s: Fail to request CHGIN in IRQ: %d: %d\n",
				__func__, sw_charger->irq_chgin, ret);
		goto err_reg_irq;
	}

	sw_charger->irq_rst = pdata->irq_base + S2MU107_SC_IRQ1_CHG_Restart;
	ret = request_threaded_irq(sw_charger->irq_rst, NULL,
			s2mu107_sc_restart_isr, 0, "restart-irq", sw_charger);
	if (ret < 0) {
		dev_err(s2mu107->dev, "%s: Fail to request CHG_Restart in IRQ: %d: %d\n",
				__func__, sw_charger->irq_rst, ret);
		goto err_reg_irq;
	}

	sw_charger->irq_done = pdata->irq_base + S2MU107_SC_IRQ1_DONE;
	ret = request_threaded_irq(sw_charger->irq_done, NULL,
			s2mu107_sc_done_isr, 0, "done-irq", sw_charger);
	if (ret < 0) {
		dev_err(s2mu107->dev, "%s: Fail to request DONE in IRQ: %d: %d\n",
				__func__, sw_charger->irq_done, ret);
		goto err_reg_irq;
	}

	sw_charger->irq_chg_fault = pdata->irq_base + S2MU107_SC_IRQ1_CHG_Fault;
	ret = request_threaded_irq(sw_charger->irq_chg_fault, NULL,
			s2mu107_sc_fault_isr, 0, "chg_fault-irq", sw_charger);
	if (ret < 0) {
		dev_err(s2mu107->dev, "%s: Fail to request CHG_Fault in IRQ: %d: %d\n",
				__func__, sw_charger->irq_chg_fault, ret);
		goto err_reg_irq;
	}
	sw_charger->irq_otg = pdata->irq_base + S2MU107_SC_IRQ2_OTG;
	ret = request_threaded_irq(sw_charger->irq_otg, NULL,
			s2mu107_otg_isr, 0, "otg-irq", sw_charger);
	if (ret < 0) {
		dev_err(s2mu107->dev, "%s: Fail to request OTG in IRQ: %d: %d\n",
				__func__, sw_charger->irq_otg, ret);
		goto err_reg_irq;
	}

	INIT_DELAYED_WORK(&sw_charger->otg_vbus_work, s2mu107_otg_vbus_work);

#if EN_TEST_READ
	s2mu107_sc_test_read(sw_charger->i2c);
#endif
	pr_info("%s:[BATT] S2MU107 switching charger driver loaded OK\n", __func__);

	return 0;

err_reg_irq:
	destroy_workqueue(sw_charger->charger_wqueue);
err_create_wq:
	power_supply_unregister(sw_charger->psy_otg);
err_power_supply_register_otg:
	power_supply_unregister(sw_charger->psy_sc);
err_power_supply_register:
err_parse_dt:
err_parse_dt_nomem:
	mutex_destroy(&sw_charger->charger_mutex);
	kfree(sw_charger);
	return ret;
}

static int s2mu107_switching_charger_remove(struct platform_device *pdev)
{
	struct s2mu107_sc_data *sw_charger =
		platform_get_drvdata(pdev);

	power_supply_unregister(sw_charger->psy_sc);
	mutex_destroy(&sw_charger->charger_mutex);
	kfree(sw_charger);
	return 0;
}

#if defined CONFIG_PM
static int s2mu107_switching_charger_suspend(struct device *dev)
{
	return 0;
}

static int s2mu107_switching_charger_resume(struct device *dev)
{
	return 0;
}
#else
#define s2mu107_switching_charger_suspend NULL
#define s2mu107_switching_charger_resume NULL
#endif

static void s2mu107_switching_charger_shutdown(struct device *dev)
{
	pr_info("%s: S2MU107 switching charger driver shutdown\n", __func__);
}

static SIMPLE_DEV_PM_OPS(s2mu107_sw_charger_pm_ops, s2mu107_switching_charger_suspend,
		s2mu107_switching_charger_resume);

static struct platform_driver s2mu107_switching_charger_driver = {
	.driver         = {
		.name   = "s2mu107-switching-charger",
		.owner  = THIS_MODULE,
		.of_match_table = s2mu107_sw_charger_match_table,
		.pm     = &s2mu107_sw_charger_pm_ops,
		.shutdown   =   s2mu107_switching_charger_shutdown,
	},
	.probe          = s2mu107_switching_charger_probe,
	.remove     = s2mu107_switching_charger_remove,
};

static int __init s2mu107_switching_charger_init(void)
{
	int ret = 0;
	pr_info("%s start\n", __func__);
	ret = platform_driver_register(&s2mu107_switching_charger_driver);

	return ret;
}
module_init(s2mu107_switching_charger_init);

static void __exit s2mu107_switching_charger_exit(void)
{
	platform_driver_unregister(&s2mu107_switching_charger_driver);
}
module_exit(s2mu107_switching_charger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("switching charger driver for S2MU107");

