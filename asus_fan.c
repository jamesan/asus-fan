/**
 *  ASUS Fan control module
 *
 *  PLEASE USE WITH CAUTION, you can easily overheat your machine with a wrong
 *  manually set fan speed...
 *
**/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/device.h>

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>

MODULE_AUTHOR("Felipe Contreras <felipe.contreras@gmail.com>");
MODULE_AUTHOR("Markus Meissner <coder@safemailbox.de>");
MODULE_AUTHOR("Bernd Kast <kastbernd@gmx.de>");
MODULE_DESCRIPTION("ASUS fan driver (ACPI)");
MODULE_LICENSE("GPL");

// These defines are taken from asus-nb-wmi
#define to_platform_driver(drv) \
  (container_of((drv), struct platform_driver, driver))
#define to_asus_fan_driver(pdrv) \
  (container_of((pdrv), struct asus_fan_driver, platform_driver))

#define DRIVER_NAME "asus_fan"
#define ASUS_FAN_VERSION "#MODULE_VERSION#"

#define TEMP1_CRIT 105
#define TEMP1_LABEL "gfx_temp"

struct asus_fan_driver {
  const char *name;
  struct module *owner;

  int (*probe)(struct platform_device *device);

  struct platform_driver platform_driver;
  struct platform_device *platform_device;
};

struct asus_fan {
  struct platform_device *platform_device;

  struct asus_fan_driver *driver;
  struct asus_fan_driver *driver_gfx;
};

//////
////// GLOBALS
//////

// 'fan_states' save last (manually) set fan state/speed
static int fan_states[2] = {-1, -1};
// 'fan_manual_mode' keeps whether this fan is manually controlled
static bool fan_manual_mode[2] = {false, false};

// 'true' - if current system was identified and thus a second fan is available
static bool has_gfx_fan;

// params struct used frequently for acpi-call-construction
static struct acpi_object_list params;

// max fan speed default
static int max_fan_speed_default = 255;
// ... user-defined max value
static int max_fan_speed_setting = 255;

//// fan "name"
// regular fan name
static char *fan_desc = "CPU Fan";
// gfx-card fan name
static char *gfx_fan_desc = "GFX Fan";

// this speed will be reported as the minimal speed for the fans
static int fan_minimum = 10;
static int fan_minimum_gfx = 10;

static struct asus_fan_driver asus_fan_driver = {
    .name = DRIVER_NAME, .owner = THIS_MODULE,
};
bool used;

static struct attribute *platform_attributes[] = {NULL};
static struct attribute_group platform_attribute_group = {
    .attrs = platform_attributes};

//////
////// FUNCTION PROTOTYPES
//////

// hidden fan api funcs used for both (wrap into them)
static int __fan_get_cur_state(int fan, unsigned long *state);
static int __fan_set_cur_state(int fan, unsigned long state);

// get current mode (auto, manual, perhaps auto mode of module in future)
static int __fan_get_cur_control_state(int fan, int *state);
// switch between modes (auto, manual, perhaps auto mode of module in future)
static int __fan_set_cur_control_state(int fan, int state);

// regular fan api funcs
static ssize_t fan_get_cur_state(struct device *dev,
                                 struct device_attribute *attr, char *buf);
static ssize_t fan_set_cur_state(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count);

// gfx fan api funcs
static ssize_t fan_get_cur_state_gfx(struct device *dev,
                                     struct device_attribute *attr, char *buf);
static ssize_t fan_set_cur_state_gfx(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count);

// regular fan api funcs
static ssize_t fan_get_cur_control_state(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf);
static ssize_t fan_set_cur_control_state(struct device *dev,
                                         struct device_attribute *attr,
                                         const char *buf, size_t count);

// gfx fan api funcs
static ssize_t fan_get_cur_control_state_gfx(struct device *dev,
                                             struct device_attribute *attr,
                                             char *buf);
static ssize_t fan_set_cur_control_state_gfx(struct device *dev,
                                             struct device_attribute *attr,
                                             const char *buf, size_t count);

// generic fan func (no sense as long as auto-mode is bound to both or none of
// the fans...
// - force 'reset' of max-speed (if reset == true) and change to auto-mode
static int fan_set_max_speed(unsigned long state, bool reset);
// acpi-readout
static int fan_get_max_speed(unsigned long *state);

// set fan(s) to automatic mode
static int fan_set_auto(void);

// set fan with index 'fan' to 'speed'
// - includes manual mode activation
static int fan_set_speed(int fan, int speed);

// reports current speed of the fan (unit:RPM)
static int __fan_rpm(int fan);

// Writes RPMs of fan0 (CPU fan) to buf => needed for hwmon device
static ssize_t fan_rpm(struct device *dev, struct device_attribute *attr,
                       char *buf);

// Writes RPMs of fan1 (GPU fan) to buf => needed for hwmon device
static ssize_t fan_rpm_gfx(struct device *dev, struct device_attribute *attr,
                           char *buf);
// Writes Label of fan0 (CPU fan) to buf => needed for hwmon device
static ssize_t fan_label(struct device *dev, struct device_attribute *attr,
                         char *buf);

// Writes Label of fan1 (GPU fan) to buf => needed for hwmon device
static ssize_t fan_label_gfx(struct device *dev, struct device_attribute *attr,
                             char *buf);
// Writes Minimal speed of fan0 (CPU fan) to buf => needed for hwmon device
static ssize_t fan_min(struct device *dev, struct device_attribute *attr,
                       char *buf);

// Writes Minimal speed of fan1 (GPU fan) to buf => needed for hwmon device
static ssize_t fan_min_gfx(struct device *dev, struct device_attribute *attr,
                           char *buf);

// sets maximal speed for auto and manual mode => needed for hwmon device
static ssize_t set_max_speed(struct device *dev, struct device_attribute *attr,
                             const char *buf, size_t count);

// writes maximal speed for auto and manual mode to buf => needed for hwmon
// device
static ssize_t get_max_speed(struct device *dev, struct device_attribute *attr,
                             char *buf);

// GFX temperature
static ssize_t temp1_input(struct device *dev, struct device_attribute *attr,
                           char *buf);
// GFX label
static ssize_t temp1_crit(struct device *dev, struct device_attribute *attr,
                           char *buf);
// GFX crit
static ssize_t temp1_label(struct device *dev, struct device_attribute *attr,
                           char *buf);

// is the hwmon interface visible?
static umode_t asus_hwmon_sysfs_is_visible(struct kobject *kobj,
                                           struct attribute *attr, int idx);

// initialization of hwmon interface
static int asus_fan_hwmon_init(struct asus_fan *asus);

// remove "asus_fan" subfolder from /sys/devices/platform
static void asus_fan_sysfs_exit(struct platform_device *device);

// set up platform device and call hwmon init
static int asus_fan_probe(struct platform_device *pdev);

// do anything needed to remove platform device
static int asus_fan_remove(struct platform_device *device);

// prepare platform device and let it create
int __init_or_module asus_fan_register_driver(struct asus_fan_driver *driver);

// remove the driver
void asus_fan_unregister_driver(struct asus_fan_driver *driver);

// housekeeping (module) stuff...
static void __exit fan_exit(void);
static int __init fan_init(void);

//////
////// IMPLEMENTATIONS
//////
static int __fan_get_cur_state(int fan, unsigned long *state) {

  /* very nasty, but (by now) the only idea I had to calculate the pwm value
from the measured pwms
   * => heat up the notebook
   * => reduce maxumum fan speed
   * => rpms are still updated and you know the pwm value => Mapping Table
   * => do a regression
   * => =RPM*RPM*0,0000095+0,01028*RPM+26,5
RPMs	PWM
3640	190
3500	180
3370	170
3240	160
3110	150
2960	140
2800	130
2640	120
2470	110
2290	100
2090	90
1890	80
1660	70
1410	60
1110	50
950	45
790	40
*/
  int rpm = __fan_rpm(fan);

  if (fan_manual_mode[fan]) {
    *state = fan_states[fan];
  } else {
    if (rpm == 0) {
      *state = 0;
      return 0;
    }
    *state = rpm *rpm * 100 / 10526316 + rpm * 1000 / 97276 + 26;
    // ensure state is within a valid range
    if (*state > 255) {
      *state = 0;
    }
  }
  return 0;
}

static int __fan_set_cur_state(int fan, unsigned long state) {

  // catch illegal state set
  if (state > 255) {
    printk(KERN_INFO "asus-fan (set pwm%d) - illegal value provided: %d \n",
           fan, (unsigned int) state);
    return 1;
  }

  fan_states[fan] = state;
  fan_manual_mode[fan] = true;
  return fan_set_speed(fan, state);
}

static int __fan_get_cur_control_state(int fan, int *state) {
  *state = fan_manual_mode[fan];
  return 0;
}

static int __fan_set_cur_control_state(int fan, int state) {
  if (state == 0) {
    return fan_set_auto();
  }
  return 0;
}

static int fan_set_speed(int fan, int speed) {
  // struct acpi_object_list params;
  union acpi_object args[2];
  unsigned long long value;

  // set speed to 'speed' for given 'fan'-index
  // -> automatically switch to manual mode!
  params.count = ARRAY_SIZE(args);
  params.pointer = args;
  // Args:
  // fan index
  // - add '1' to index as '0' has a special meaning (auto-mode)
  args[0].type = ACPI_TYPE_INTEGER;
  args[0].integer.value = fan + 1;
  // target fan speed
  // - between 0x00 and MAX (0 - MAX)
  //   - 'MAX' is usually 0xFF (255)
  //   - should be getable with fan_get_max_speed()
  args[1].type = ACPI_TYPE_INTEGER;
  args[1].integer.value = speed;
  // acpi call
  return acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.SFNV", &params,
                               &value);
}

static int __fan_rpm(int fan) {
  struct acpi_object_list params;
  union acpi_object args[1];
  unsigned long long value;
  acpi_status ret;

  // fan does not report during manual speed setting - so fake it!
  if (fan_manual_mode[fan]) {
    value = fan_states[fan] * fan_states[fan] * 1000 / -16054 +
            fan_states[fan] * 32648 / 1000 - 365;
    if (value > 10000)
      return 0;
  } else {

    // getting current fan 'speed' as 'state',
    params.count = ARRAY_SIZE(args);
    params.pointer = args;
    // Args:
    // - get speed from the fan with index 'fan'
    args[0].type = ACPI_TYPE_INTEGER;
    args[0].integer.value = fan;

    // acpi call
    ret = acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.TACH", &params,
                                &value);
    if (ret != AE_OK)
      return -1;
  }
  return (int)value;
}
static ssize_t fan_rpm(struct device *dev, struct device_attribute *attr,
                       char *buf) {
  return sprintf(buf, "%d\n", __fan_rpm(0));
}
static ssize_t fan_rpm_gfx(struct device *dev, struct device_attribute *attr,
                           char *buf) {
  return sprintf(buf, "%d\n", __fan_rpm(1));
}

static ssize_t fan_get_cur_state(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  unsigned long state = 0;
  __fan_get_cur_state(0, &state);
  return sprintf(buf, "%lu\n", state);
}

static ssize_t fan_get_cur_state_gfx(struct device *dev,
                                     struct device_attribute *attr, char *buf) {
  unsigned long state = 0;
  __fan_get_cur_state(1, &state);
  return sprintf(buf, "%lu\n", state);
}

static ssize_t fan_set_cur_state_gfx(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count) {
  int state;
  kstrtouint(buf, 10, &state);
  __fan_set_cur_state(1, state);
  return count;
}

static ssize_t fan_set_cur_state(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count) {
  int state;
  kstrtouint(buf, 10, &state);
  __fan_set_cur_state(0, state);
  return count;
}

static ssize_t fan_get_cur_control_state(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf) {
  int state = 0;
  __fan_get_cur_control_state(0, &state);
  return sprintf(buf, "%d\n", state);
}

static ssize_t fan_get_cur_control_state_gfx(struct device *dev,
                                             struct device_attribute *attr,
                                             char *buf) {
  int state = 0;
  __fan_get_cur_control_state(1, &state);
  return sprintf(buf, "%d\n", state);
}

static ssize_t fan_set_cur_control_state_gfx(struct device *dev,
                                             struct device_attribute *attr,
                                             const char *buf, size_t count) {
  int state;
  kstrtouint(buf, 10, &state);
  __fan_set_cur_control_state(1, state);
  return count;
}

static ssize_t fan_set_cur_control_state(struct device *dev,
                                         struct device_attribute *attr,
                                         const char *buf, size_t count) {
  int state;
  kstrtouint(buf, 10, &state);
  __fan_set_cur_control_state(0, state);
  return count;
}

// Reading the correct max fan speed does not work!
// Setting a max value has the obvious effect, thus we 'fake'
// the 'get_max' function
static int fan_get_max_speed(unsigned long *state) {

  *state = max_fan_speed_setting;
  return 0;
}

static int fan_set_max_speed(unsigned long state, bool reset) {
  union acpi_object args[1];
  unsigned long long value;
  acpi_status ret;
  int arg_qmod = 1;

  // if reset is 'true' ignore anything else and reset to
  // -> auto-mode with max-speed
  // -> use "SB.ARKD.QMOD" _without_ "SB.QFAN",
  //    which seems not writeable as expected
  if (reset) {
    state = 255;
    arg_qmod = 2;
    // Activate the set maximum speed setting
    // Args:
    // 0 - just returns
    // 1 - sets quiet mode to QFAN value
    // 2 - sets quiet mode to 0xFF (that's the default value)
    params.count = ARRAY_SIZE(args);
    params.pointer = args;
    // pass arg
    args[0].type = ACPI_TYPE_INTEGER;
    args[0].integer.value = arg_qmod;

    // acpi call
    ret = acpi_evaluate_integer(NULL, "\\_SB.ATKD.QMOD", &params, &value);
    if (ret != AE_OK) {
      printk(KERN_INFO
             "asus-fan (set_max_speed) - set max fan speed(s) failed (force "
             "reset)! errcode: %d",
             ret);
      return ret;
    }

    // if reset was not forced, set max fan speed to 'state'
  } else {
    // is applied automatically on any available fan
    // - docs say it should affect manual _AND_ automatic mode
    // Args:
    // - from 0x00 to 0xFF (0 - 255)
    params.count = ARRAY_SIZE(args);
    params.pointer = args;
    // pass arg
    args[0].type = ACPI_TYPE_INTEGER;
    args[0].integer.value = state;

    // acpi call
    ret = acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.ST98", &params,
                                &value);
    if (ret != AE_OK) {
      printk(KERN_INFO
             "asus-fan (set_max_speed) - set max fan speed(s) failed (no "
             "reset)! errcode: %d",
             ret);
      return ret;
    }
  }

  // keep set max fan speed for the get_max
  max_fan_speed_setting = state;

  return ret;
}

static int fan_set_auto() {
  union acpi_object args[2];
  unsigned long long value;
  acpi_status ret;

  // setting (both) to auto-mode simultanously
  fan_manual_mode[0] = false;
  fan_states[0] = -1;
  if (has_gfx_fan) {
    fan_states[1] = -1;
    fan_manual_mode[1] = false;
  }

  // acpi call to call auto-mode for all fans!
  params.count = ARRAY_SIZE(args);
  params.pointer = args;
  // special fan-id == 0 must be used
  args[0].type = ACPI_TYPE_INTEGER;
  args[0].integer.value = 0;
  // speed has to be set to zero
  args[1].type = ACPI_TYPE_INTEGER;
  args[1].integer.value = 0;

  // acpi call
  ret =
      acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.SFNV", &params, &value);
  if (ret != AE_OK) {
    printk(KERN_INFO
           "asus-fan (set_auto) - failed reseting fan(s) to auto-mode! "
           "errcode: %d - DANGER! OVERHEAT? DANGER!",
           ret);
    return ret;
  }

  return ret;
}

static ssize_t fan_label(struct device *dev, struct device_attribute *attr,
                         char *buf) {
  return sprintf(buf, "%s\n", fan_desc);
}

static ssize_t fan_label_gfx(struct device *dev, struct device_attribute *attr,
                             char *buf) {
  return sprintf(buf, "%s\n", gfx_fan_desc);
}

static ssize_t fan_min(struct device *dev, struct device_attribute *attr,
                       char *buf) {
  return sprintf(buf, "%d\n", fan_minimum);
}

static ssize_t fan_min_gfx(struct device *dev, struct device_attribute *attr,
                           char *buf) {
  return sprintf(buf, "%d\n", fan_minimum_gfx);
}

static ssize_t set_max_speed(struct device *dev, struct device_attribute *attr,
                             const char *buf, size_t count) {
  int state;
  bool reset = false;
  kstrtouint(buf, 10, &state);
  if (state == 256) {
    reset = true;
  }
  fan_set_max_speed(state, reset);
  return count;
}

static ssize_t get_max_speed(struct device *dev, struct device_attribute *attr,
                             char *buf) {
  unsigned long state = 0;
  fan_get_max_speed(&state);
  return sprintf(buf, "%lu\n", state);
}

static ssize_t temp1_input(struct device *dev, struct device_attribute *attr,
                           char *buf) {
    acpi_status ret;
    unsigned long long int value;

    // acpi call
    ret = acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.TH1R", NULL, &value);

    return sprintf(buf, "%llud\n", value*1000);
}

static ssize_t temp1_label(struct device *dev, struct device_attribute *attr,
                           char *buf) {
  return sprintf(buf, "%s\n", TEMP1_LABEL);
}

static ssize_t temp1_crit(struct device *dev, struct device_attribute *attr,
                           char *buf) {
  return sprintf(buf, "%d\n", TEMP1_CRIT);
}



// Makros defining all possible hwmon attributes
static DEVICE_ATTR(pwm1, S_IWUSR | S_IRUGO, fan_get_cur_state,
                   fan_set_cur_state);
static DEVICE_ATTR(pwm1_enable, S_IWUSR | S_IRUGO, fan_get_cur_control_state,
                   fan_set_cur_control_state);
static DEVICE_ATTR(fan1_min, S_IRUGO, fan_min, NULL);
static DEVICE_ATTR(fan1_input, S_IRUGO, fan_rpm, NULL);
static DEVICE_ATTR(fan1_label, S_IRUGO, fan_label, NULL);

static DEVICE_ATTR(fan1_speed_max, S_IWUSR | S_IRUGO, get_max_speed,
                   set_max_speed);

static DEVICE_ATTR(pwm2, S_IWUSR | S_IRUGO, fan_get_cur_state_gfx,
                   fan_set_cur_state_gfx);
static DEVICE_ATTR(pwm2_enable, S_IWUSR | S_IRUGO,
                   fan_get_cur_control_state_gfx,
                   fan_set_cur_control_state_gfx);
static DEVICE_ATTR(fan2_min, S_IRUGO, fan_min_gfx, NULL);
static DEVICE_ATTR(fan2_input, S_IRUGO, fan_rpm_gfx, NULL);
static DEVICE_ATTR(fan2_label, S_IRUGO, fan_label_gfx, NULL);

static DEVICE_ATTR(temp1_input, S_IRUGO, temp1_input, NULL);
static DEVICE_ATTR(temp1_label, S_IRUGO, temp1_label, NULL);
static DEVICE_ATTR(temp1_crit, S_IRUGO, temp1_crit, NULL);

// hwmon attributes without second fan
static struct attribute *hwmon_attributes[] = {
    &dev_attr_pwm1.attr,           &dev_attr_pwm1_enable.attr,
    &dev_attr_fan1_min.attr,       &dev_attr_fan1_input.attr,
    &dev_attr_fan1_label.attr,

    &dev_attr_fan1_speed_max.attr,

    &dev_attr_temp1_input.attr,
    &dev_attr_temp1_label.attr,
    &dev_attr_temp1_crit.attr,
    NULL};

// hwmon attributes with second fan
static struct attribute *hwmon_gfx_attributes[] = {
    &dev_attr_pwm1.attr,           &dev_attr_pwm1_enable.attr,
    &dev_attr_fan1_min.attr,       &dev_attr_fan1_input.attr,
    &dev_attr_fan1_label.attr,

    &dev_attr_fan1_speed_max.attr,

    &dev_attr_pwm2.attr,           &dev_attr_pwm2_enable.attr,
    &dev_attr_fan2_min.attr,       &dev_attr_fan2_input.attr,
    &dev_attr_fan2_label.attr,     

    &dev_attr_temp1_input.attr,
    &dev_attr_temp1_label.attr,
    &dev_attr_temp1_crit.attr,
    NULL};

// by now sysfs is always visible
static umode_t asus_hwmon_sysfs_is_visible(struct kobject *kobj,
                                           struct attribute *attr, int idx) {
	return attr->mode;
}

static struct attribute_group hwmon_attribute_group = {
    .is_visible = asus_hwmon_sysfs_is_visible, .attrs = hwmon_attributes};
__ATTRIBUTE_GROUPS(hwmon_attribute);

static struct attribute_group hwmon_gfx_attribute_group = {
    .is_visible = asus_hwmon_sysfs_is_visible, .attrs = hwmon_gfx_attributes};
__ATTRIBUTE_GROUPS(hwmon_gfx_attribute);

static int asus_fan_hwmon_init(struct asus_fan *asus) {
  struct device *hwmon;
  if (!has_gfx_fan) {
    hwmon = hwmon_device_register_with_groups(
        &asus->platform_device->dev, "asus_fan", asus, hwmon_attribute_groups);
    if (IS_ERR(hwmon)) {
      pr_err("Could not register asus hwmon device\n");
      return PTR_ERR(hwmon);
    }
  } else {
    hwmon = hwmon_device_register_with_groups(&asus->platform_device->dev,
                                              "asus_fan", asus,
                                              hwmon_gfx_attribute_groups);
    if (IS_ERR(hwmon)) {
      pr_err("Could not register asus hwmon device\n");
      return PTR_ERR(hwmon);
    }
  }
  return 0;
}

static void asus_fan_sysfs_exit(struct platform_device *device) {
  sysfs_remove_group(&device->dev.kobj, &platform_attribute_group);
}

static int asus_fan_probe(struct platform_device *pdev) {
  struct platform_driver *pdrv = to_platform_driver(pdev->dev.driver);
  struct asus_fan_driver *wdrv = to_asus_fan_driver(pdrv);

  struct asus_fan *asus;
  int err = 0;

  asus = kzalloc(sizeof(struct asus_fan), GFP_KERNEL);
  if (!asus)
    return -ENOMEM;

  asus->driver = wdrv;
  asus->platform_device = pdev;
  wdrv->platform_device = pdev;
  platform_set_drvdata(asus->platform_device, asus);

  sysfs_create_group(&asus->platform_device->dev.kobj,
                     &platform_attribute_group);

  err = asus_fan_hwmon_init(asus);
  if (err)
    goto fail_hwmon;
  return 0;

fail_hwmon:
  asus_fan_sysfs_exit(asus->platform_device);
  kfree(asus);
  return err;
}

static int asus_fan_remove(struct platform_device *device) {
  struct asus_fan *asus;

  asus = platform_get_drvdata(device);
  asus_fan_sysfs_exit(asus->platform_device);
  kfree(asus);
  return 0;
}

int __init_or_module asus_fan_register_driver(struct asus_fan_driver *driver) {
  struct platform_driver *platform_driver;
  struct platform_device *platform_device;

  if (used) {
    return -EBUSY;
  }
  platform_driver = &driver->platform_driver;
  platform_driver->remove = asus_fan_remove;
  platform_driver->driver.owner = driver->owner;
  platform_driver->driver.name = driver->name;

  platform_device =
      platform_create_bundle(platform_driver, asus_fan_probe, NULL, 0, NULL, 0);
  if (IS_ERR(platform_device)) {
    return PTR_ERR(platform_device);
  }

  used = true;
  return 0;
}

static int __init fan_init(void) {
  acpi_status ret;
  int rpm;
  // identify system/model/platform
  if (!strcmp(dmi_get_system_info(DMI_SYS_VENDOR), "ASUSTeK COMPUTER INC.")) {

    rpm = __fan_rpm(0);
    if (rpm == -1)
      return -ENODEV;
    rpm = __fan_rpm(1);
    if (rpm == -1)
      has_gfx_fan = false;
    else
      has_gfx_fan = true;
    // check if reseting fan speeds works
    ret = fan_set_max_speed(max_fan_speed_default, false);
    if (ret != AE_OK) {
      printk(KERN_INFO
             "asus-fan (init) - set max speed to: '%d' failed! errcode: %d",
             max_fan_speed_default, ret);
      return -ENODEV;
    }

    // force sane enviroment / init with automatic fan controlling
    if ((ret = fan_set_auto()) != AE_OK) {
      printk(KERN_INFO
             "asus-fan (init) - set auto-mode speed to active, failed! "
             "errcode: %d",
             ret);
      return -ENODEV;
    }

    ret = asus_fan_register_driver(&asus_fan_driver);
    if (ret != AE_OK) {
      printk(KERN_INFO
             "asus-fan (init) - set max speed to: '%d' failed! errcode: %d",
             max_fan_speed_default, ret);
      return ret;
    }
  }
  printk(KERN_INFO "asus-fan (init) - finished init\n");
  return 0;
}

void asus_fan_unregister_driver(struct asus_fan_driver *driver) {
  platform_device_unregister(driver->platform_device);
  platform_driver_unregister(&driver->platform_driver);
  used = false;
}

static void __exit fan_exit(void) {
  fan_set_auto();
  asus_fan_unregister_driver(&asus_fan_driver);
  used = false;

  printk(KERN_INFO "asus-fan (exit) - module unloaded - cleaning up...\n");
}

module_init(fan_init);
module_exit(fan_exit);
