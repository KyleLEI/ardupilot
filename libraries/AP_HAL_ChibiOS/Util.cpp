/*
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Code by Andrew Tridgell and Siddharth Bharat Purohit
 */
#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>

#include "Util.h"
#include <ch.h>
#include "RCOutput.h"
#include "hwdef/common/stm32_util.h"
#include "hwdef/common/watchdog.h"
#include "hwdef/common/flash.h"
#include <AP_ROMFS/AP_ROMFS.h>
#include <AP_Common/ExpandingString.h>
#include "sdcard.h"
#include "shared_dma.h"
#include <AP_Common/ExpandingString.h>

#if HAL_ENABLE_SAVE_PERSISTENT_PARAMS
#include <AP_InertialSensor/AP_InertialSensor.h>
#endif

#if HAL_WITH_IO_MCU
#include <AP_BoardConfig/AP_BoardConfig.h>
#include <AP_IOMCU/AP_IOMCU.h>
extern AP_IOMCU iomcu;
#endif

extern const AP_HAL::HAL& hal;

using namespace ChibiOS;
#if CH_CFG_USE_HEAP == TRUE

/**
   how much free memory do we have in bytes.
*/
uint32_t Util::available_memory(void)
{
    // from malloc.c in hwdef
    return mem_available();
}

/*
    Special Allocation Routines
*/

void* Util::malloc_type(size_t size, AP_HAL::Util::Memory_Type mem_type)
{
    if (mem_type == AP_HAL::Util::MEM_DMA_SAFE) {
        return malloc_dma(size);
    } else if (mem_type == AP_HAL::Util::MEM_FAST) {
        return malloc_fastmem(size);
    } else {
        return calloc(1, size);
    }
}

void Util::free_type(void *ptr, size_t size, AP_HAL::Util::Memory_Type mem_type)
{
    if (ptr != NULL) {
        free(ptr);
    }
}


#ifdef ENABLE_HEAP

void *Util::allocate_heap_memory(size_t size)
{
    void *buf = malloc(size);
    if (buf == nullptr) {
        return nullptr;
    }

    memory_heap_t *heap = (memory_heap_t *)malloc(sizeof(memory_heap_t));
    if (heap != nullptr) {
        chHeapObjectInit(heap, buf, size);
    }

    return heap;
}

/*
  realloc implementation thanks to wolfssl, used by AP_Scripting
 */
void *Util::std_realloc(void *addr, size_t size)
{
    if (size == 0) {
       free(addr);
       return nullptr;
    }
    if (addr == nullptr) {
        return malloc(size);
    }
    void *new_mem = malloc(size);
    if (new_mem != nullptr) {
        memcpy(new_mem, addr, chHeapGetSize(addr) > size ? size : chHeapGetSize(addr));
        free(addr);
    }
    return new_mem;
}

void *Util::heap_realloc(void *heap, void *ptr, size_t new_size)
{
    if (heap == nullptr) {
        return nullptr;
    }
    if (new_size == 0) {
        if (ptr != nullptr) {
            chHeapFree(ptr);
        }
        return nullptr;
    }
    if (ptr == nullptr) {
        return chHeapAlloc((memory_heap_t *)heap, new_size);
    }
    void *new_mem = chHeapAlloc((memory_heap_t *)heap, new_size);
    if (new_mem != nullptr) {
        memcpy(new_mem, ptr, chHeapGetSize(ptr) > new_size ? new_size : chHeapGetSize(ptr));
        chHeapFree(ptr);
    }
    return new_mem;
}
#endif // ENABLE_HEAP

#endif // CH_CFG_USE_HEAP

/*
  get safety switch state
 */
Util::safety_state Util::safety_switch_state(void)
{
#if HAL_USE_PWM == TRUE
    return ((RCOutput *)hal.rcout)->_safety_switch_state();
#else
    return SAFETY_NONE;
#endif
}

#ifdef HAL_PWM_ALARM
struct Util::ToneAlarmPwmGroup Util::_toneAlarm_pwm_group = HAL_PWM_ALARM;

bool Util::toneAlarm_init()
{
    _toneAlarm_pwm_group.pwm_cfg.period = 1000;
    pwmStart(_toneAlarm_pwm_group.pwm_drv, &_toneAlarm_pwm_group.pwm_cfg);

    return true;
}

void Util::toneAlarm_set_buzzer_tone(float frequency, float volume, uint32_t duration_ms)
{
    if (is_zero(frequency) || is_zero(volume)) {
        pwmDisableChannel(_toneAlarm_pwm_group.pwm_drv, _toneAlarm_pwm_group.chan);
    } else {
        pwmChangePeriod(_toneAlarm_pwm_group.pwm_drv,
                        roundf(_toneAlarm_pwm_group.pwm_cfg.frequency/frequency));

        pwmEnableChannel(_toneAlarm_pwm_group.pwm_drv, _toneAlarm_pwm_group.chan, roundf(volume*_toneAlarm_pwm_group.pwm_cfg.frequency/frequency)/2);
    }
}
#endif // HAL_PWM_ALARM

/*
  set HW RTC in UTC microseconds
*/
void Util::set_hw_rtc(uint64_t time_utc_usec)
{
    stm32_set_utc_usec(time_utc_usec);
}

/*
  get system clock in UTC microseconds
*/
uint64_t Util::get_hw_rtc() const
{
    return stm32_get_utc_usec();
}

#if !defined(HAL_NO_FLASH_SUPPORT) && !defined(HAL_NO_ROMFS_SUPPORT)

#if defined(HAL_NO_GCS) || defined(HAL_BOOTLOADER_BUILD)
#define Debug(fmt, args ...)  do { hal.console->printf(fmt, ## args); } while (0)
#else
#include <GCS_MAVLink/GCS.h>
#define Debug(fmt, args ...)  do { gcs().send_text(MAV_SEVERITY_INFO, fmt, ## args); } while (0)
#endif

Util::FlashBootloader Util::flash_bootloader()
{
    uint32_t fw_size;
    const char *fw_name = "bootloader.bin";

    EXPECT_DELAY_MS(11000);

    const uint8_t *fw = AP_ROMFS::find_decompress(fw_name, fw_size);
    if (!fw) {
        Debug("failed to find %s\n", fw_name);
        return FlashBootloader::NOT_AVAILABLE;
    }
    // make sure size is multiple of 32
    fw_size = (fw_size + 31U) & ~31U;

    bool uptodate = true;
    const uint32_t addr = hal.flash->getpageaddr(0);

    if (memcmp(fw, (const void*)addr, fw_size) != 0) {
        uptodate = false;
    }

#if HAL_ENABLE_SAVE_PERSISTENT_PARAMS
    // see if we should store persistent parameters along with the
    // bootloader. We only do this on boards using a single sector for
    // the bootloader. The persistent parameters are stored as text at
    // the end of the sector
    const int32_t space_available = hal.flash->getpagesize(0) - int32_t(fw_size);
    ExpandingString persistent_params {}, old_persistent_params {};
    if (get_persistent_params(persistent_params) &&
        space_available >= persistent_params.get_length() &&
        (!load_persistent_params(old_persistent_params) ||
         strcmp(persistent_params.get_string(),
                old_persistent_params.get_string()) != 0)) {
        // persistent parameters have changed, we will update
        // bootloader to allow storage of the params
        uptodate = false;
    }
#endif

    if (uptodate) {
        Debug("Bootloader up-to-date\n");
        AP_ROMFS::free(fw);
        return FlashBootloader::NO_CHANGE;
    }

    Debug("Erasing\n");
    uint32_t erased_size = 0;
    uint8_t erase_page = 0;
    while (erased_size < fw_size) {
        uint32_t page_size = hal.flash->getpagesize(erase_page);
        if (page_size == 0) {
            AP_ROMFS::free(fw);
            return FlashBootloader::FAIL;
        }
        hal.scheduler->expect_delay_ms(1000);
        if (!hal.flash->erasepage(erase_page)) {
            Debug("Erase %u failed\n", erase_page);
            AP_ROMFS::free(fw);
            return FlashBootloader::FAIL;
        }
        erased_size += page_size;
        erase_page++;
    }

    Debug("Flashing %s @%08x\n", fw_name, (unsigned int)addr);
    const uint8_t max_attempts = 10;
    hal.flash->keep_unlocked(true);
    for (uint8_t i=0; i<max_attempts; i++) {
        hal.scheduler->expect_delay_ms(1000);
        bool ok = hal.flash->write(addr, fw, fw_size);
        if (!ok) {
            Debug("Flash failed! (attempt=%u/%u)\n",
                                i+1,
                                max_attempts);
            hal.scheduler->delay(100);
            continue;
        }
        Debug("Flash OK\n");
#if HAL_ENABLE_SAVE_PERSISTENT_PARAMS
        if (persistent_params.get_length()) {
            const uint32_t ofs = hal.flash->getpagesize(0) - persistent_params.get_length();
            hal.flash->write(addr+ofs, persistent_params.get_string(), persistent_params.get_length());
        }
#endif
        hal.flash->keep_unlocked(false);
        AP_ROMFS::free(fw);
        return FlashBootloader::OK;
    }

    hal.flash->keep_unlocked(false);
    Debug("Flash failed after %u attempts\n", max_attempts);
    AP_ROMFS::free(fw);
    return FlashBootloader::FAIL;
}
#endif // !HAL_NO_FLASH_SUPPORT && !HAL_NO_ROMFS_SUPPORT

/*
  display system identifer - board type and serial number
 */
bool Util::get_system_id(char buf[40])
{
    uint8_t serialid[12];
    char board_name[14];

    memcpy(serialid, (const void *)UDID_START, 12);
    strncpy(board_name, CHIBIOS_SHORT_BOARD_NAME, 13);
    board_name[13] = 0;

    // this format is chosen to match the format used by HAL_PX4
    snprintf(buf, 40, "%s %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
             board_name,
             (unsigned)serialid[3], (unsigned)serialid[2], (unsigned)serialid[1], (unsigned)serialid[0],
             (unsigned)serialid[7], (unsigned)serialid[6], (unsigned)serialid[5], (unsigned)serialid[4],
             (unsigned)serialid[11], (unsigned)serialid[10], (unsigned)serialid[9],(unsigned)serialid[8]);
    buf[39] = 0;
    return true;
}

bool Util::get_system_id_unformatted(uint8_t buf[], uint8_t &len)
{
    len = MIN(12, len);
    memcpy(buf, (const void *)UDID_START, len);
    return true;
}

// return true if the reason for the reboot was a watchdog reset
bool Util::was_watchdog_reset() const
{
    return stm32_was_watchdog_reset();
}

#if CH_DBG_ENABLE_STACK_CHECK == TRUE
/*
  display stack usage as text buffer for @SYS/threads.txt
 */
void Util::thread_info(ExpandingString &str)
{
  // a header to allow for machine parsers to determine format
  const uint32_t isr_stack_size = uint32_t((const uint8_t *)&__main_stack_end__ - (const uint8_t *)&__main_stack_base__);
  str.printf("ThreadsV2\nISR           PRI=255 sp=%p STACK=%u/%u\n",
             &__main_stack_base__,
             unsigned(stack_free(&__main_stack_base__)),
             unsigned(isr_stack_size));

  for (thread_t *tp = chRegFirstThread(); tp; tp = chRegNextThread(tp)) {
      uint32_t total_stack;
      if (tp->wabase == (void*)&__main_thread_stack_base__) {
          // main thread has its stack separated from the thread context
          total_stack = uint32_t((const uint8_t *)&__main_thread_stack_end__ - (const uint8_t *)&__main_thread_stack_base__);
      } else {
          // all other threads have their thread context pointer
          // above the stack top
          total_stack = uint32_t(tp) - uint32_t(tp->wabase);
      }
#if HAL_ENABLE_THREAD_STATISTICS
      str.printf("%-13.13s PRI=%3u sp=%p STACK=%4u/%4u MIN=%4u AVG=%4u MAX=%4u\n",
                 tp->name, unsigned(tp->prio), tp->wabase,
                 stack_free(tp->wabase), total_stack, RTC2US(STM32_HSECLK, tp->stats.best),
                 RTC2US(STM32_HSECLK, uint32_t(tp->stats.cumulative / uint64_t(tp->stats.n))),
                 RTC2US(STM32_HSECLK, tp->stats.worst));
      chTMObjectInit(&tp->stats); // reset counters to zero
#else
      str.printf("%-13.13s PRI=%3u sp=%p STACK=%u/%u\n",
                 tp->name, unsigned(tp->prio), tp->wabase,
                 unsigned(stack_free(tp->wabase)), unsigned(total_stack));
#endif
  }
}
#endif // CH_DBG_ENABLE_STACK_CHECK == TRUE

#if CH_CFG_USE_SEMAPHORES
// request information on dma contention
void Util::dma_info(ExpandingString &str)
{
    ChibiOS::Shared_DMA::dma_info(str);
}
#endif

#if HAL_ENABLE_SAVE_PERSISTENT_PARAMS

static const char *persistent_header = "{{PERSISTENT_START_V1}}\n";

/*
  create a set of persistent parameters in string form
 */
bool Util::get_persistent_params(ExpandingString &str) const
{
    str.printf("%s", persistent_header);
#if HAL_INS_TEMPERATURE_CAL_ENABLE
    const auto *ins = AP_InertialSensor::get_singleton();
    if (ins) {
        ins->get_persistent_params(str);
    }
#endif
    if (str.has_failed_allocation() || str.get_length() <= strlen(persistent_header)) {
        // no data
        return false;
    }
    // ensure that the length is a multiple of 32 to meet flash alignment requirements
    while (!str.has_failed_allocation() && str.get_length() % 32 != 0) {
        str.append(" ", 1);
    }
    return !str.has_failed_allocation();
}

/*
  load a set of persistent parameters in string form from the bootloader sector
 */
bool Util::load_persistent_params(ExpandingString &str) const
{
    const uint32_t addr = hal.flash->getpageaddr(0);
    const uint32_t size = hal.flash->getpagesize(0);
    const char *s = (const char *)memmem((void*)addr, size,
                                         persistent_header,
                                         strlen(persistent_header));
    if (s) {
        str.append(s, (addr+size) - uint32_t(s));
        return !str.has_failed_allocation();
    }
    return false;
}

/*
  apply persistent parameters from the bootloader sector to AP_Param
 */
void Util::apply_persistent_params(void) const
{
    ExpandingString str {};
    if (!load_persistent_params(str)) {
        return;
    }
    char *s = str.get_writeable_string();
    char *saveptr;
    s += strlen(persistent_header);
    uint32_t count = 0;
    for (char *p = strtok_r(s, "\n", &saveptr);
         p; p = strtok_r(nullptr, "\n", &saveptr)) {
        char *eq = strchr(p, int('='));
        if (eq) {
            *eq = 0;
            if (AP_Param::set_default_by_name(p, atof(eq+1))) {
                count++;
            }
        }
    }
    if (count) {
        AP_Param::invalidate_count();
        GCS_SEND_TEXT(MAV_SEVERITY_INFO, "Loaded %u persistent parameters",
                      unsigned(count));
    }
}

#endif // HAL_ENABLE_SAVE_PERSISTENT_PARAMS

