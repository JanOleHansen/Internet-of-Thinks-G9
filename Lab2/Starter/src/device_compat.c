#include <errno.h>
#include <stdbool.h>

struct device;

extern bool z_device_ready(const struct device *dev);
extern const struct device *z_impl_device_get_binding(const char *name);

int device_usable_check(const struct device *dev)
{
	return z_device_ready(dev) ? 0 : -ENODEV;
}

const struct device *device_get_binding(const char *name)
{
	return z_impl_device_get_binding(name);
}
