/*
 * Copyright (C) 2014 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>

#include <hardware/power.h>
#include <fcntl.h>

#define LOG_TAG "PowerHAL"
#include <utils/Log.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <hardware/hardware.h>
#include <semaphore.h>
#include "CGroupCpusetController.h"
#include "DevicePowerMonitor.h"

#define ENABLE 1
#define TOUCHBOOST_PULSE_SYSFS "/sys/devices/system/cpu/cpufreq/interactive/touchboostpulse"
static const char cpufreq_boost_interactive[] = "/sys/devices/system/cpu/cpufreq/interactive/boost";
static const char cpufreq_boost_intel_pstate[] = "/sys/devices/system/cpu/intel_pstate/min_perf_pct";

/*
 * This parameter is to identify continuous touch/scroll events.
 * Any two touch hints received between a 20 interval ms is
 * considered as a scroll event.
 */
#define SHORT_TOUCH_TIME 20
#define SCHEDTUNE_BOOST_PATH "/dev/stune/foreground/schedtune.boost"
#define SCHEDTUNE_BOOST_NORM "10"
#define SCHEDTUNE_BOOST_INTERACTIVE "40"
#define SCHEDTUNE_BOOST_TIME_NS 1000000000LL
#define container_of(addr, struct_name, field_name) \
    ((struct_name *)((char *)(addr) - offsetof(struct_name, field_name)))

/*
 * This parameter is to identify first touch events.
 * Any two touch hints received after 100 ms is considered as
 * a first touch event.
 */
#define LONG_TOUCH_TIME 100

/*
 * This parameter defines the number of vsync boost to be
 * done after the finger release event.
 */
#define VSYNC_BOOST_COUNT 4

/*
 * This parameter defines the time between a touch and a vsync
 * hint. the time if is > 30 ms, we do a vsync boost.
 */
#define VSYNC_TOUCH_TIME 30

static CGroupCpusetController cgroupCpusetController;
static DevicePowerMonitor powerMonitor;
static bool serviceRegistered = false;
static bool interactiveActive = false;
static bool intelPStateActive = false;
static bool intelSchedBoostActive = false;

struct intel_power_module{
    struct power_module container;
    int touchboost_disable;
    int timer_set;
    int vsync_boost;
    pthread_mutex_t lock;
    int schedtune_boost_fd;
    long long deboost_time;
    sem_t signal_lock;
};

static int sysfs_write(const char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return -1;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int sysfs_read(const char *path, char *s, int length)
{
    char buf[80];
    int len;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return -1;
    }

    len = read(fd, s, length);
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error reading from %s: %s\n", path, buf);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

#ifdef APP_LAUNCH_BOOST
static void app_launch_boost_interactive(void *hint_data)
{
    if (hint_data != NULL) {
        ALOGI("PowerHAL HAL:App Boost ON");
        sysfs_write(cpufreq_boost_interactive,(char *)"1");
    } else {
        ALOGI("PowerHAL HAL:App Boost OFF");
        sysfs_write(cpufreq_boost_interactive,(char *)"0");
    }
}

static void app_launch_boost_intel_pstate(void *hint_data)
{
    static char old_min_perf_pct[4];
    static bool boosted = false;

    if (hint_data != NULL) {
        ALOGI("PowerHAL HAL:App Boost ON");
        if (boosted == false) {
            if (!sysfs_read(cpufreq_boost_intel_pstate, old_min_perf_pct, sizeof(old_min_perf_pct))) {
                sysfs_write(cpufreq_boost_intel_pstate,(char *)"100");
                boosted = true;
            }
        }
    } else {
        ALOGI("PowerHAL HAL:App Boost OFF");
        if (boosted == true) {
            sysfs_write(cpufreq_boost_intel_pstate, old_min_perf_pct);
            boosted = false;
        }
    }
}

#endif

#define NSEC_PER_SEC 1000000000LL
static long long gettime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void nanosleep_ns(long long ns)
{
    struct timespec ts;
    ts.tv_sec = ns/NSEC_PER_SEC;
    ts.tv_nsec = ns%NSEC_PER_SEC;
    nanosleep(&ts, NULL);
}

int schedtune_sysfs_boost(struct intel_power_module *intel, char* booststr)
{
    char buf[80];
    int len;
    len = write(intel->schedtune_boost_fd, booststr, 2);

    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", SCHEDTUNE_BOOST_PATH, buf);
    }
    return len;
}

static void* schedtune_deboost_thread(void* arg)
{
    struct intel_power_module *intel = (struct intel_power_module *)arg;
    while(1) {
        sem_wait(&intel->signal_lock);
        while(1) {
            long long now, sleeptime = 0;
            pthread_mutex_lock(&intel->lock);
            now = gettime_ns();
            if (intel->deboost_time > now) {
                sleeptime = intel->deboost_time - now;
                pthread_mutex_unlock(&intel->lock);
                nanosleep_ns(sleeptime);
		continue;
            }

            schedtune_sysfs_boost(intel,(char*)SCHEDTUNE_BOOST_NORM);
            intel->deboost_time = 0;
            pthread_mutex_unlock(&intel->lock);
            break;
        }
    }
    return NULL;
}

static void schedtune_boost(struct intel_power_module *intel)
{
    long long now;
    now = gettime_ns();

    if (!intel->deboost_time) {
        schedtune_sysfs_boost(intel,(char*)SCHEDTUNE_BOOST_INTERACTIVE);
        sem_post(&intel->signal_lock);
    }
    intel->deboost_time = now + SCHEDTUNE_BOOST_TIME_NS;
    return;
}

static int schedtune_power_init(struct intel_power_module *intel)
{
    char buf[50];
    pthread_t tid;
    intel->deboost_time = 0;
    sem_init(&intel->signal_lock, 0, 1);
    intel->schedtune_boost_fd = open(SCHEDTUNE_BOOST_PATH, O_WRONLY);

    if (intel->schedtune_boost_fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", SCHEDTUNE_BOOST_PATH, buf);
        sem_close(&intel->signal_lock);
        return -1;
    }
    pthread_create(&tid, NULL, schedtune_deboost_thread, intel);
    return 0;
}

static void power_init(__attribute__((unused))struct power_module *module)
{
    int cnt = 0;
    char buf[1];
    struct intel_power_module *intel = container_of(module,struct intel_power_module, container);

    /* Enable all devices by default */
    powerMonitor.setState(ENABLE);
    cgroupCpusetController.setState(ENABLE);

    if (!sysfs_read(TOUCHBOOST_PULSE_SYSFS, buf, 1))
        interactiveActive = true;
    if (!sysfs_read(cpufreq_boost_intel_pstate, buf, 1))
	intelPStateActive = true;
    if (!schedtune_power_init(intel))
	intelSchedBoostActive = true;
}

static void power_set_interactive(__attribute__((unused))struct power_module *module, int on)
{
    powerMonitor.setState(on);
    cgroupCpusetController.setState(on);
}

static void power_hint(struct power_module *module, power_hint_t hint,
                       void *data)
{
    struct intel_power_module *intel = (struct intel_power_module *) module;
    static struct timespec curr_time, prev_time = {0,0}, vsync_time;
    double diff;
    static int vsync_count;
    static int consecutive_touch_int;

    pthread_mutex_lock(&intel->lock);
    switch(hint) {
    case POWER_HINT_INTERACTION:
        if (!interactiveActive) {
            if (!intelSchedBoostActive) {
                break;
          } else {
                schedtune_boost(intel);
                break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &curr_time);
        diff = (curr_time.tv_sec - prev_time.tv_sec) * 1000 +
               (double)(curr_time.tv_nsec - prev_time.tv_nsec) / 1e6;
        prev_time = curr_time;
        if (diff < SHORT_TOUCH_TIME) {
            consecutive_touch_int++;
        }
        else if (diff > LONG_TOUCH_TIME) {
            intel->vsync_boost = 0;
            intel->timer_set = 0;
            intel->touchboost_disable = 0;
            vsync_count = 0;
            consecutive_touch_int = 0;
        }
        /* Simple touch: timer rate need not be changed here */
        if ((diff < SHORT_TOUCH_TIME) && (intel->touchboost_disable == 0)
                        && (consecutive_touch_int > 4))
            intel->touchboost_disable = 1;
        /*
         * Scrolling: timer rate reduced to increase sensitivity. No more touch
         * boost after this
         */
        if ((intel->touchboost_disable == 1) && (consecutive_touch_int > 15)
                        && (intel->timer_set == 0)) {
           intel->timer_set = 1;
        }
        if (!intel->touchboost_disable) {
            sysfs_write(TOUCHBOOST_PULSE_SYSFS, (char *)"1");
        }
        break;
    case POWER_HINT_VSYNC:
        if (!interactiveActive) {
            pthread_mutex_unlock(&intel->lock);
            return;
        }
        if (intel->touchboost_disable == 1) {
            clock_gettime(CLOCK_MONOTONIC, &vsync_time);
            diff = (vsync_time.tv_sec - curr_time.tv_sec) * 1000 +
            (double)(vsync_time.tv_nsec - curr_time.tv_nsec) / 1e6;
            if (diff > VSYNC_TOUCH_TIME) {
                intel->timer_set = 0;
                intel->vsync_boost = 1;
                intel->touchboost_disable = 0;
                vsync_count = VSYNC_BOOST_COUNT;
            }
        }
        if (intel->vsync_boost) {
            if (((unsigned long)data != 0) && (vsync_count > 0)) {
                sysfs_write(TOUCHBOOST_PULSE_SYSFS, (char *)"1");
                vsync_count-- ;
            if (vsync_count == 0)
               intel->vsync_boost = 0;
            }
        }
        break;
    case POWER_HINT_LOW_POWER:
        break;

#if defined(APP_LAUNCH_BOOST) && defined(APP_LAUNCH_BOOST_SUPPORTED)
    case POWER_HINT_APP_LAUNCH:
        if (interactiveActive)
            app_launch_boost_interactive(data);
	else if (intelPStateActive)
            app_launch_boost_intel_pstate(data);
#endif
    default:
        break;
    }
    pthread_mutex_unlock(&intel->lock);
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct intel_power_module HAL_MODULE_INFO_SYM = {
    container:{
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = POWER_MODULE_API_VERSION_0_2,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "Intel PC Compatible Power HAL",
            .author = "Intel Open Source Technology Center",
            .methods = &power_module_methods,
            .dso = NULL,
            .reserved = {},
        },
    .init = power_init,
    .setInteractive = power_set_interactive,
    .powerHint = power_hint,
    },
    touchboost_disable: 0,
    timer_set: 0,
    vsync_boost: 0,
};
