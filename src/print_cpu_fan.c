// vim:ts=4:sw=4:expandtab
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <yajl/yajl_gen.h>
#include <yajl/yajl_version.h>

#include "i3status.h"

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <err.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#define TZ_ZEROC 2732
#define TZ_KELVTOC(x) (((x)-TZ_ZEROC) / 10), abs(((x)-TZ_ZEROC) % 10)
#define TZ_AVG(x) ((x)-TZ_ZEROC) / 10
#endif

#if defined(__DragonFly__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/sensors.h>
#define MUKTOC(v) ((v - 273150000) / 1000000.0)
#endif

#if defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/sensors.h>
#include <errno.h>
#include <err.h>

#define MUKTOC(v) ((v - 273150000) / 1000000.0)
#endif

#if defined(__NetBSD__)
#include <fcntl.h>
#include <prop/proplib.h>
#include <sys/envsys.h>

#define MUKTOC(v) ((v - 273150000) / 1000000.0)
#endif

/*
 * Reads the CPU fan from /sys/devices/virtual/hwmon/hwmon%d/fan%d_input (or
 * the user provided path) and returns the temperature in degree celcius.
 *
 */
void print_cpu_fan_info(yajl_gen json_gen, char *buffer, int zone, const char *path, const char *format, int min_threshold) {
    char *outwalk = buffer;
#ifdef THERMAL_ZONE
    const char *walk;
    bool colorful_output = false;
    char *thermal_zone;

    if (path == NULL)
        asprintf(&thermal_zone, THERMAL_ZONE, zone);
    else
        asprintf(&thermal_zone, path, zone);

    INSTANCE(thermal_zone);

    for (walk = format; *walk != '\0'; walk++) {
        if (*walk != '%') {
            *(outwalk++) = *walk;
            continue;
        }

        if (BEGINS_WITH(walk + 1, "rpm")) {
#if defined(LINUX)
            static char buf[16];
            long int temp;
            if (!slurp(thermal_zone, buf, sizeof(buf)))
                goto error;
            temp = strtol(buf, NULL, 10);
            if (temp == LONG_MIN || temp == LONG_MAX || temp <= 0)
                *(outwalk++) = '0';
            else {
                if ((temp) <= min_threshold) {
                    START_COLOR("color_bad");
                    colorful_output = true;
                }
                outwalk += sprintf(outwalk, "%ld", (temp));
                if (colorful_output) {
                    END_COLOR;
                    colorful_output = false;
                }
            }
#elif defined(__DragonFly__)
            struct sensor th_sensor;
            size_t th_sensorlen;

            th_sensorlen = sizeof(th_sensor);

            if (sysctlbyname(thermal_zone, &th_sensor, &th_sensorlen, NULL, 0) == -1) {
                perror("sysctlbyname");
                goto error;
            }
            if (MUKTOC(th_sensor.value) <= min_threshold) {
                START_COLOR("color_bad");
                colorful_output = true;
            }
            outwalk += sprintf(outwalk, "%.2f", MUKTOC(th_sensor.value));
            if (colorful_output) {
                END_COLOR;
                colorful_output = false;
            }

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
            int sysctl_rslt;
            size_t sysctl_size = sizeof(sysctl_rslt);
            if (sysctlbyname(thermal_zone, &sysctl_rslt, &sysctl_size, NULL, 0))
                goto error;

            if (TZ_AVG(sysctl_rslt) <= min_threshold) {
                START_COLOR("color_bad");
                colorful_output = true;
            }
            outwalk += sprintf(outwalk, "%d.%d", TZ_KELVTOC(sysctl_rslt));
            if (colorful_output) {
                END_COLOR;
                colorful_output = false;
            }

#elif defined(__OpenBSD__)
            struct sensordev sensordev;
            struct sensor sensor;
            size_t sdlen, slen;
            int dev, numt, mib[5] = {CTL_HW, HW_SENSORS, 0, 0, 0};

            sdlen = sizeof(sensordev);
            slen = sizeof(sensor);

            for (dev = 0;; dev++) {
                mib[2] = dev;
                if (sysctl(mib, 3, &sensordev, &sdlen, NULL, 0) == -1) {
                    if (errno == ENXIO)
                        continue;
                    if (errno == ENOENT)
                        break;
                    goto error;
                }
                /* 'path' is the node within the full path (defaults to acpitz0). */
                if (BEGINS_WITH(sensordev.xname, thermal_zone)) {
                    mib[3] = SENSOR_TEMP;
                    /* Limit to temo0, but should retrieve from a full path... */
                    for (numt = 0; numt < 1 /*sensordev.maxnumt[SENSOR_TEMP]*/; numt++) {
                        mib[4] = numt;
                        if (sysctl(mib, 5, &sensor, &slen, NULL, 0) == -1) {
                            if (errno != ENOENT) {
                                warn("sysctl");
                                continue;
                            }
                        }
                        if ((int)MUKTOC(sensor.value) <= min_threshold) {
                            START_COLOR("color_bad");
                            colorful_output = true;
                        }

                        outwalk += sprintf(outwalk, "%.2f", MUKTOC(sensor.value));

                        if (colorful_output) {
                            END_COLOR;
                            colorful_output = false;
                        }
                    }
                }
            }
#elif defined(__NetBSD__)
            int fd, rval;
            bool err = false;
            prop_dictionary_t dict;
            prop_array_t array;
            prop_object_iterator_t iter;
            prop_object_iterator_t iter2;
            prop_object_t obj, obj2, obj3;

            fd = open("/dev/sysmon", O_RDONLY);
            if (fd == -1)
                goto error;

            rval = prop_dictionary_recv_ioctl(fd, ENVSYS_GETDICTIONARY, &dict);
            if (rval == -1) {
                err = true;
                goto error_netbsd1;
            }

            /* No drivers registered? */
            if (prop_dictionary_count(dict) == 0) {
                err = true;
                goto error_netbsd2;
            }

            iter = prop_dictionary_iterator(dict);
            if (iter == NULL) {
                err = true;
                goto error_netbsd2;
            }

            /* iterate over the dictionary returned by the kernel */
            while ((obj = prop_object_iterator_next(iter)) != NULL) {
                /* skip this dict if it's not what we're looking for */
                if ((strlen(prop_dictionary_keysym_cstring_nocopy(obj)) != strlen(thermal_zone)) ||
                    (strncmp(thermal_zone,
                             prop_dictionary_keysym_cstring_nocopy(obj),
                             strlen(thermal_zone)) != 0))
                    continue;

                array = prop_dictionary_get_keysym(dict, obj);
                if (prop_object_type(array) != PROP_TYPE_ARRAY) {
                    err = true;
                    goto error_netbsd3;
                }

                iter2 = prop_array_iterator(array);
                if (!iter2) {
                    err = true;
                    goto error_netbsd3;
                }

                /* iterate over array of dicts specific to target sensor */
                while ((obj2 = prop_object_iterator_next(iter2)) != NULL) {
                    obj3 = prop_dictionary_get(obj2, "cur-value");

                    float temp = MUKTOC(prop_number_integer_value(obj3));
                    if ((int)temp <= min_threshold) {
                        START_COLOR("color_bad");
                        colorful_output = true;
                    }

                    outwalk += sprintf(outwalk, "%.2f", temp);

                    if (colorful_output) {
                        END_COLOR;
                        colorful_output = false;
                    }

                    break;
                }
                prop_object_iterator_release(iter2);
            }
        error_netbsd3:
            prop_object_iterator_release(iter);
        error_netbsd2:
            prop_object_release(dict);
        error_netbsd1:
            close(fd);
            if (err)
                goto error;

#endif

            walk += strlen("rpm");
        }
    }

    free(thermal_zone);

    OUTPUT_FULL_TEXT(buffer);
    return;
error:
    free(thermal_zone);
#endif

    OUTPUT_FULL_TEXT("can't read fan speed");
    (void)fputs("i3status: Cannot read fan speed. Verify that you have a virtual hardware mon in /sys/devices/virtual/hwmon or disable the cpu_fan module in your i3status config.\n", stderr);
}
