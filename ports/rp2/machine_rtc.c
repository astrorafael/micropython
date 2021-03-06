/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2021 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "lib/utils/mpirq.h"

// pico-sdk includes
#include "pico/util/datetime.h"
#include "hardware/rtc.h"

#define RTC_ALARM0 (0)
#define RP2_PWR_MODE_IDLE         (0x01)
#define RP2_PWR_MODE_SLEEP        (0x02)
#define RP2_PWR_MODE_DEEPSLEEP    (0x04)

enum DAY_OF_THE_WEEK {Sunday = 0, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday};

// MicroPython ref datetime: Jan, Thursday, 1st, 2015
#define MJD_BASE      736012
#define MJD_DOTW_BASE Thursday
typedef struct _machine_rtc_obj_t {
    mp_obj_base_t base;
    bool active;        // active alarm flag
    mp_uint_t period;   // in seconds. 0 = no periodic
    mp_uint_t alarm;    // alarm time in seconds since 2015, Jan. 1st
} machine_rtc_obj_t;


// From https://pdc.ro.nu/jd-code.html by Robin O'Leary
// Adapted for minimal computation, only accounts for leap years
STATIC mp_uint_t calendar_to_mjd(mp_uint_t year, mp_uint_t month, mp_uint_t day) {
    if (month < 3) {
        year--;
        month += 12;
    }
    return (year * 365) + (year / 4) + (month * 153 + 3) / 5 + day;
}

STATIC mp_uint_t to_seconds(const datetime_t *t) {
    mp_uint_t days;
    days = calendar_to_mjd(t->year, t->month, t->day) - MJD_BASE;
    return days * 24 * 60 * 60 + t->hour * 3600 + t->min * 60 + t->sec;
}


const mp_obj_type_t machine_rtc_type;

// singleton RTC object
STATIC machine_rtc_obj_t machine_rtc_obj = {
    .base = {&machine_rtc_type},
    .active = false,
    .period = 0,
    .alarm = 0,
};


// class machine.RTC(id=0, ...) Constructor
mp_obj_t machine_rtc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_id };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id, MP_ARG_REQUIRED | MP_ARG_OBJ },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t rtc_id = mp_obj_get_int(args[ARG_id].u_obj);
    if (rtc_id != 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("RTC(%d) doesn't exist"), rtc_id);
    }
    return (mp_obj_t)&machine_rtc_obj;
}


// RTC.init(datetime) method
// tuple is (year, month, day[, hour[, minute[, second[, microsecond[, tzinfo]]]]])
STATIC mp_obj_t machine_rtc_init(mp_obj_t self_in, mp_obj_t datetime) {
    mp_obj_t *items;
    mp_obj_get_array_fixed_n(datetime, 8, &items);
    mp_int_t year = mp_obj_get_int(items[0]);
    mp_int_t month = mp_obj_get_int(items[1]);
    mp_int_t day = mp_obj_get_int(items[2]);
    datetime_t t = {
        .year = year,   // 16 bits
        .month = month, // 8 bits
        .day = day,     // 8 bits
        .dotw = ((calendar_to_mjd(year, month, day) - MJD_BASE) % 7) + MJD_DOTW_BASE,   // 8 bits
        .hour = mp_obj_get_int(items[3]),  // 8 bits
        .min = mp_obj_get_int(items[4]),   // 8 bits
        .sec = mp_obj_get_int(items[5]),   // 8 bits
    };
    // ports/rp2/main.c already calls rtc_init(), so we do no init RTC here
    rtc_set_datetime(&t);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(machine_rtc_init_obj, machine_rtc_init);


// RTC.deinit() method
// sets the date iome as per the MicroPython official documentation
STATIC mp_obj_t machine_rtc_deinit(mp_obj_t self_in) {
    datetime_t t = {
        .year = 2015,
        .month = 1,
        .day = 1,
        .hour = 0,
        .min = 0,
        .sec = 0,
        .dotw = Thursday,
    };
    rtc_set_datetime(&t);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(machine_rtc_deinit_obj, machine_rtc_deinit);


// RTC.now() method
STATIC mp_obj_t machine_rtc_now(mp_obj_t self_in) {
    datetime_t t;
    rtc_get_datetime(&t);
    mp_obj_t tuple[8] = {
        mp_obj_new_int(t.year),
        mp_obj_new_int(t.month),
        mp_obj_new_int(t.day),
        mp_obj_new_int(t.hour),
        mp_obj_new_int(t.min),
        mp_obj_new_int(t.sec),
        mp_obj_new_int(0),  // usecs
        mp_const_none       // tzinfo
    };
    return mp_obj_new_tuple(8, tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(machine_rtc_now_obj, machine_rtc_now);


// RTC.alarm(id, time, *, repeat=False) method
STATIC mp_obj_t machine_rtc_alarm(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_id, ARG_time, ARG_repeat};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,     MP_ARG_INT,                    {.u_int = (0)}},
        { MP_QSTR_time,   MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_rom_obj = MP_ROM_NONE}},
        { MP_QSTR_repeat, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false}},
    };
    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("id must be 0"));
    }
    if (self->active) {
        mp_raise_msg(&mp_type_Exception, MP_ERROR_TEXT("RTC(alarm_id=0) already active"));
    }
    datetime_t now;
    rtc_get_datetime(&now);
    mp_uint_t now_secs = to_seconds(&now);
    mp_uint_t duration, later_secs;
    // handle datetime as duration or absulute timestamp
    if (mp_obj_is_int(args[ARG_time].u_obj)) {
        bool periodic = args[ARG_repeat].u_bool;
        duration = mp_obj_get_int(args[ARG_time].u_obj) / 1000;
        self->period = periodic ? duration : 0;
        later_secs = now_secs + duration;
    } else {
        datetime_t later;
        mp_obj_t *tstamp;
        mp_obj_get_array_fixed_n(args[ARG_time].u_obj, 8, &tstamp);
        later.year = mp_obj_get_int(tstamp[0]);
        later.month = mp_obj_get_int(tstamp[1]);
        later.day = mp_obj_get_int(tstamp[2]);
        later.hour = mp_obj_get_int(tstamp[3]);
        later.min = mp_obj_get_int(tstamp[4]);
        later.sec = mp_obj_get_int(tstamp[5]);
        later_secs = to_seconds(&later);
        if (later_secs <= now_secs) {
            mp_raise_ValueError(MP_ERROR_TEXT("time already passed"));
        }
        duration = later_secs - now_secs;
    }
    self->alarm = later_secs;
    self->active = true;
    return mp_obj_new_int_from_uint(duration * 1000);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_alarm_obj, 1, machine_rtc_alarm);


// RTC.alarm_left(alarm_id=0)  method
STATIC mp_obj_t machine_rtc_alarm_left(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_alarm_id};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_alarm_id, MP_ARG_INT, {.u_int = (0)}},

    };
    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_alarm_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("alarm_id must be 0"));
    }
    if (!self->active) {
        mp_raise_msg(&mp_type_Exception, MP_ERROR_TEXT("RTC(alarm_id=0) not active"));
    }
    datetime_t t;
    rtc_get_datetime(&t);
    mp_uint_t current = to_seconds(&t);
    mp_uint_t limit = self->alarm;
    mp_uint_t left = current >= limit ? 0 : limit - current;
    if (!left) {   // reload  if periodic and alarm expired
        if (self->period != 0) {
            left = self->period;
            self->alarm = current + self->period;
            self->active = true;
        } else {
            self->active = false;   // One shot mode
        }
    }
    return mp_obj_new_int_from_uint(left * 1000);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_alarm_left_obj, 1, machine_rtc_alarm_left);


// RTC.cancel(alarm_id=0) method
STATIC mp_obj_t machine_rtc_cancel(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_alarm_id};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_alarm_id, MP_ARG_INT, {.u_int = (0)}},

    };
    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_alarm_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("alarm_id must be 0"));
    }
    self->active = false;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_cancel_obj, 1, machine_rtc_cancel);


// RTC.irq(*, trigger, handler=None, wake=machine.IDLE) method
STATIC mp_obj_t machine_rtc_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("RTC.irq() not implemented"));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_irq_obj, 1, machine_rtc_irq);


// RTC.__repr__() && RTC.__str__() methods
STATIC void machine_rtc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    mp_printf(print, "RTC(id=0, running=%s)", rtc_running() ? "true" : "false");
}


STATIC const mp_rom_map_elem_t machine_rtc_locals_dict_table[] = {
    // Map Python methods
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&machine_rtc_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_init),    MP_ROM_PTR(&machine_rtc_init_obj)   },
    { MP_ROM_QSTR(MP_QSTR_deinit),  MP_ROM_PTR(&machine_rtc_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_now),     MP_ROM_PTR(&machine_rtc_now_obj)    },
    { MP_ROM_QSTR(MP_QSTR_alarm),      MP_ROM_PTR(&machine_rtc_alarm_obj)      },
    { MP_ROM_QSTR(MP_QSTR_alarm_left), MP_ROM_PTR(&machine_rtc_alarm_left_obj) },
    { MP_ROM_QSTR(MP_QSTR_cancel),     MP_ROM_PTR(&machine_rtc_cancel_obj)     },
    { MP_ROM_QSTR(MP_QSTR_irq),        MP_ROM_PTR(&machine_rtc_irq_obj)        },
    // Map Python Constants
    { MP_ROM_QSTR(MP_QSTR_ALARM0),  MP_ROM_INT(RTC_ALARM0) },
};
STATIC MP_DEFINE_CONST_DICT(machine_rtc_locals_dict, machine_rtc_locals_dict_table);


const mp_obj_type_t machine_rtc_type = {
    { &mp_type_type },
    .name = MP_QSTR_RTC,
    .print = machine_rtc_print,
    .make_new = machine_rtc_make_new,
    .locals_dict = (mp_obj_dict_t *)&machine_rtc_locals_dict,
};
