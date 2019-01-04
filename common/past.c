/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Johan Kanflo (github.com/kanflo)
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

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <past.h>
#include <flash.h>

#ifdef CONFIG_PAST_DEBUG
 #include "dbg_printf.h"
#endif // CONFIG_PAST_DEBUG

/*
 * Past - parameter storage (EEPROM style)
 * The Past module enables parameter storage in the internal eeprom of the STM32
 * Parameters are identified by an 32 bit integer id (0 and 0xffffffff are
 * invalid ids) and the maximum size of a parameter is 4Gb (which we obviously
 * cannot store :)
 *
 * This module originally appeared in tne OpenDPS project and is basically the
 * same with the difference that this version stores data in the STM32L0 EEPROM
 * rather than in flash. The STM32L0 EEPROM is different in that erased space
 * is 0x00 and not 0xff as the interal flash of the STM32F0. Actually, erased
 * internal flash on the STM32L0 is alseo 0x00.
 *
 * According to this scheme, we can find the end of the past by finding the unit
 * with id 0 and length 0.
 *
 * A unit can be written, rewritten or removed at randon and the module has
 * transaction support meaning that a unit will be written in its completeness
 * or not at all. If power is lost during a write, the half written unit will be
 * be removed on the next power cycle.
 *
 * Past uses the following storage format:
 *
 * [  magic:32  ] [ counter:32 ]
 * [   id:32    ] [  size:32   ] [   data+   ] [ padding <1-3 bytes> ]
 * [   id:32    ] [  size:32   ] [   data+   ] [ padding <1-3 bytes> ]
 *    .
 *    .
 *    .
 * [ 0x00000000 ] [ 0x00000000 ]
 *
 * Each of the two Past blocks begin with the Past magic, followed by a past
 * counter which is increased by one for each garbage collection. The counter is
 * never expected to wrap as the number of erase cycles is far less than a 32
 * bit integer...
 *
 * Each unit begins at an even 32 bit boundary. The unit id 0x00000000 denotes
 * end of past and cannot be used. The usage of 32 bit integers may seem
 * wasteful but has been selected as size this is the minimum writable unit
 * (MWU) on the STM32L0, for which this module is targeted. It adapting this
 * module for eg. STM32F4s, that need to change because of the MWU of 8 bytes.
 *
 * Past uses two blocks. When one block is full (it gets filled as parameters
 * are added (obviously) and rewritten) the block is compacted and rewritten
 * into the other Past block.
 *
 * * Writing a unit *
 * When writing a unit, the unit data is written first. Secondly, the size and
 * id are written and the parameter write is complete. There exists a theoretial
 * change that power is lost during this last write. If so, an attempt will be
 * made to clean up the write during the next power cycle.
 *
 * * Removing a unit *
 * When removing a unit, the id field is written as 0x00000000, the length is
 * kept and the data is overwritten with zeros. This is why 0 is not a valid
 * unit id.
 *
 * * Rewriting a unit *
 * Rewriting is not an operation in itself in terms that the user does not have
 * care about it, it is handled inside Past. It is a combination of writing
 * and removing the old unit.
 *
 * * Reading a unit *
 * When reading a unit, a pointer to the data in flash is returned along with
 * the size. The data is read only.
 *
 * * Past startup *
 * When the module is initialized, the integrity of the Past data is checked.
 * First, the module selects the current data block based on the Past counters
 * at offset 4 (assuming the Past magics are in place). Next the data is checked
 * for consistency. It should be possible to reach the end marker unit
 * (0x00000000) while parsing the data. If so, the resto of the block is checked
 * for erased data. If none erased data is found following the end marker, we
 * have found traces of a non completed write and a garbage collection is
 * performed.
 *
 * * Garbage collection *
 * As units get rewritten, Past will be filled with old unit data an at some
 * point it will be full. At this point it will perform a garbage collection,
 * copying all units to the other flash block. It will first erase that block
 * and then copy the valid data from the old block. When colpleted it will
 * update the block counter att offset 4 and at the very last write the past
 * magic at offset 0.
 *
 */

#define PAST_MAGIC 0x50617374 // "past"

/** When a unit is rewritten, the old versions id is changed to INVALUD */
#define PAST_UNIT_ID_INVALID           (0xffffffff)
/** This is the same as erased space and is used to find the end if the past */
#define PAST_UNIT_ID_END               (0x00000000)

#define HEADER_COUNTER_OFFSET     (4)
#define HEADER_FIRST_UNIT_OFFSET  (8)

#define UNIT_SIZE_OFFSET  (4)
#define UNIT_DATA_OFFSET  (8)

static uint32_t past_find_unit(past_t *past, past_id_t id);
static bool past_erase_unit_at(uint32_t address);
static bool past_garbage_collect(past_t *past);
static bool eeprom_zero(uint32_t address, uint32_t length);
static bool eeprom_write32(uint32_t address, uint32_t data);
#ifndef CONFIG_PAST_MINIMAL
static bool copy_parameters(past_t *past, uint32_t src_base, uint32_t dst_base);
#endif // CONFIG_PAST_MINIMAL
static uint32_t past_remaining_size(past_t *past);
#define eeprom_read32(address) *(uint32_t*) (address)

/**
  * @brief Initialize the past, format or garbage collect if needed
  * @param past A past structure with the block[] vector initialized
  * @retval true if the past could be initialized
  *         false if init falied
  */
bool past_init(past_t *past)
{
    bool success = false;
    if (past && past->blocks[0] && past->blocks[1]) {
        /** Check which block is the current one */
        uint32_t magic_1 = eeprom_read32(past->blocks[0]);
        uint32_t magic_2 = eeprom_read32(past->blocks[1]);
        uint32_t counter_1 = eeprom_read32(past->blocks[0] + HEADER_COUNTER_OFFSET);
        uint32_t counter_2 = eeprom_read32(past->blocks[1] + HEADER_COUNTER_OFFSET);
        success = true;
        if (PAST_MAGIC == magic_1 && PAST_MAGIC == magic_2) {
            past->_cur_block = counter_2 > counter_1 ? 1 : 0;
            past->_counter = counter_2 > counter_1 ? counter_2 : counter_1;
        } else if (PAST_MAGIC == magic_2) {
            past->_cur_block = 1;
            past->_counter = counter_2;
        } else if (PAST_MAGIC == magic_1) {
            past->_cur_block = 0;
            past->_counter = counter_1;
        } else {
            /** No valid Past in either block */
            past->_cur_block = 0;
            past->_counter = 0;
            success &= past_format(past);
        }
        if (success) {
            past->_end_addr = past_find_unit(past, PAST_UNIT_ID_END);
            past->_valid = true;
        }

        /** Now check all space following the end address is erased space, if not
          * we have a half completed write operation we need to clear */
        uint32_t check_addr = past->_end_addr;
        while (check_addr < past->blocks[ past->_cur_block ] + past->_block_size) {
            if (eeprom_read32(check_addr) != PAST_UNIT_ID_END) {
                success &= past_garbage_collect(past);
                break;
            }
            check_addr += 4;
        }

    }
    return success;
}

/**
  * @brief Read unit from past
  * @param past An initialized past structure
  * @param id Unit id to read
  * @param data Pointer to data read
  * @param length Length of data read
  * @retval true if the unit was found and could be read
  *         false if the unit was not found
  */
bool past_read_unit(past_t *past, past_id_t id, const void **data, uint32_t *length)
{
    if (!past || !data || !length) {
        return false;
    }
    uint32_t address = past_find_unit(past, id);
    if (address) {
        *length = eeprom_read32(address + UNIT_SIZE_OFFSET);
        *data = (const void*) address + UNIT_DATA_OFFSET;
    }
    return address ? true : false;
}

/**
  * @brief Write unit to past
  * @param past An initialized past structure
  * @param id Unit id to write
  * @param data Data to write
  * @param length Size of data
  * @retval true if the unit was written
  *         false if writing failed or the past was full
  */
bool past_write_unit(past_t *past, past_id_t id, void *data, uint32_t length)
{
    if (!past || !data || !length || id == PAST_UNIT_ID_INVALID || id == PAST_UNIT_ID_END) {
        return false;
    }
    uint32_t end_address;
    uint32_t wi = 0; /** word index */
    uint32_t temp;
    bool success = false;
    uint8_t *p = (uint8_t*) data;

    if (past_remaining_size(past) < UNIT_DATA_OFFSET + length) {
        if (!past_garbage_collect(past)) {
            return false;
        }
    }
    if (past_remaining_size(past) < UNIT_DATA_OFFSET + length) {
        return false;
    }
    end_address = past->_end_addr;
    do {
        /** Check if there is an old version of the unit */
        uint32_t old_addr = past_find_unit(past, id);
        /** Write the new unit */
        if (!eeprom_write32(end_address+UNIT_SIZE_OFFSET, length)) {
            break;
        }
        if (length >= 4) {
            while(4*wi < length) {
                /** data is not guaranteed to be word aligned */
                temp = p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
                p += 4;
                if (!eeprom_write32(end_address+UNIT_DATA_OFFSET+4*wi, temp)) {
                    break;
                }
                wi++;
                if (length % 4 && 4*(wi+1) >= length) {
                    /** Data not even multiple of 4, reading one more word would
                      * cause an out of bound buffer read */
                    break;
                }
            }
        }
        if (length % 4) { /** Write remaining 1..3 bytes */
            temp = 0;
            for (uint32_t i = 0; i < length % 4; i++) {
                uint8_t b = ((uint8_t*)(data))[4*wi + i];
                temp |= b << (8*i);
            }
            if (!eeprom_write32(end_address+UNIT_DATA_OFFSET+4*wi, temp)) {
                break;
            }
        }
        if (!eeprom_write32(end_address, id)) {
            break;
        }
        /** Update end addres of the past struct */
        end_address += UNIT_DATA_OFFSET + length;
        if (end_address % 4) {
            end_address += 4 - (end_address % 4); // Word align
        }
        past->_end_addr = end_address;

        /** If existing, erase the old version */
        if (old_addr) {
            if (!past_erase_unit_at(old_addr)) {
                break;
            }
        }
        success = true;
    } while(0);
    return success;
}

/**
  * @brief Erase unit
  * @param past An initialized past structure
  * @param id Unit id to erase
  * @retval true if the unit was found and erasing was successful
  *         false if erasing failed or the unit was not found
  */
bool past_erase_unit(past_t *past, past_id_t id)
{
    if (!past || id == PAST_UNIT_ID_INVALID || id == PAST_UNIT_ID_END) {
        return false;
    }
    bool success = false;
    do {
        uint32_t address = past_find_unit(past, id);
        if (!address) {
            break;
        }
        if (!past_erase_unit_at(address)) {
            break;
        }
        success = true;
    } while(0);
    return success;
}

/**
  * @brief Format the past area (both blocks) and initialize the first one
  * @param past pointer to an initialized past structure
  * @retval True if formatting was successful
  *         False in case of unrecoverable errors
  */
bool past_format(past_t *past)
{
    bool success = false;
    if (!past) {
        return success;
    }
    do {
        uint32_t cur_base;
        eeprom_zero(past->blocks[0], past->_block_size);
        eeprom_zero(past->blocks[1], past->_block_size);
        past->_cur_block = 0;
        past->_counter = 0;
        past->_end_addr = past->blocks[0] + HEADER_FIRST_UNIT_OFFSET;
        cur_base = past->blocks[past->_cur_block];
        if (!eeprom_write32(cur_base + HEADER_COUNTER_OFFSET, past->_counter)) {
            break;
        }
        if (!eeprom_write32(cur_base, PAST_MAGIC)) {
            break;
        }
        success = true;
    } while(0);
    return success;
}

/**
  * @brief Return size of past data in bytes
  * @param past pointer to an initialized past structure
  * @retval Past size in bytes
  * @todo Return size or "garbage collected" size?
  */
static uint32_t past_remaining_size(past_t *past)
{
    if (!past) {
        return 0;
    } else {
        return past->_block_size - (past->_end_addr - past->blocks[past->_cur_block]);
    }
}

/**
  * @brief Erase past unit at given address (points to id)
  * @param past pointer to an initialized past structure
  * @retval True if erasing was successful
  *         False in case of unrecoverable errors
  */
static bool past_erase_unit_at(uint32_t address)
{
    bool success = true;
    uint32_t length = eeprom_read32(address + UNIT_SIZE_OFFSET);
    do {
        /** Wipe data, always an even multiple of 4 bytes */
        for (uint32_t i = 0; i < length/4 && success; i++) {
            success &= eeprom_write32(address + UNIT_DATA_OFFSET + 4*i, 0);
        }
        /** Wipe unit id */
        success &= eeprom_write32(address, PAST_UNIT_ID_INVALID);
    } while(0);
    return success;
}

/**
  * @brief Find unit and return address
  * @param past pointer to an initialized past structure
  * @param id id of unit to search for
  * @retval address of unit or 0 if not found/an error occured
  */
static uint32_t past_find_unit(past_t *past, past_id_t id)
{
    uint32_t base = past->blocks[past->_cur_block];
    uint32_t cur_address = base + HEADER_FIRST_UNIT_OFFSET;
    uint32_t cur_id, cur_size;
    do {
        cur_id = eeprom_read32(cur_address);
#ifdef CONFIG_PAST_DEBUG
        dbg_printf("Found %d ", cur_id);
        dbg_printf("at 0x%08x\n", cur_address);
#endif // CONFIG_PAST_DEBUG
        if (id == cur_id) {
            break;
        } else if (cur_id == PAST_UNIT_ID_END) {
            cur_address = 0; /** Reached end */
            break;
        } else { /** Move on */
            cur_size = eeprom_read32(cur_address + UNIT_SIZE_OFFSET);
#ifdef CONFIG_PAST_DEBUG
            dbg_printf(" Size %d\n", cur_size);
#endif // CONFIG_PAST_DEBUG
            if (cur_size == 0) {
                cur_address = 0; /** Fatal error */
                break;
            }
            if (cur_size % 4 ) {
                cur_size += 4 - (cur_size % 4); // Word align
            }
            cur_address += UNIT_DATA_OFFSET + cur_size;
        }

    } while (cur_address < base + past->_block_size);
    return cur_address;
}

/**
  * @brief Perform garbage collection
  * @param past pointer to an initialized past structure
  * @retval true if GC was successful
  */
static bool past_garbage_collect(past_t *past)
{
#ifdef CONFIG_PAST_MINIMAL
    (void) past;
    return false;
#else // CONFIG_PAST_MINIMAL
    bool success = false;
    do {
        /** Format the new block */
        uint32_t new_block = past->blocks[past->_cur_block ? 0 : 1];
        uint32_t old_block = past->blocks[past->_cur_block];
        eeprom_zero(new_block, past->_block_size);
        if (!copy_parameters(past, old_block, new_block)) {
            break;
        }
        if (!eeprom_write32(new_block + HEADER_COUNTER_OFFSET, past->_counter+1)) {
            break;
        }
        if (!eeprom_write32(new_block, PAST_MAGIC)) {
            break;
        }
        eeprom_zero(old_block, past->_block_size);
        past->_counter++;
        past->_cur_block = past->_cur_block ? 0 : 1;
        success = true;
        /** Past is now ready for writing */
    } while(0);
    return success;
#endif // CONFIG_PAST_MINIMAL
}

/**
 * @brief      Write zeros to eeprom address
 *
 * @param[in]  address  The address
 * @param[in]  length   The length in bytes (must be an even multiple of 4)
 *
 * @return     true if writing went well
 */
static bool eeprom_zero(uint32_t address, uint32_t length)
{
    bool success = false;
    if (address % 4 == 0) {
        do {
            eeprom_program_word(address, 0);
            address += 4;
            length -= 4;
        } while(length != 0);
    }
    return success;
}

/**
  * @brief Write 32 bits to flash
  * @param address address to write to
  * @param data well, data
  * @retval true if write was successfuk
  */
static bool eeprom_write32(uint32_t address, uint32_t data)
{
    bool success = false;
    if (address % 4 == 0) {
        eeprom_program_word(address, data);
       success = true;
    }
    return success;
}

/**
  * @brief Copy all valid parameters from src to dst
  * @param past pointer to an initialized past structure
  * @param src_base source base address
  * @param dst_base destination base address
  * @retval true if copying was successful
  */
#ifndef CONFIG_PAST_MINIMAL
static bool copy_parameters(past_t *past, uint32_t src_base, uint32_t dst_base)
{
    bool success = true;
    uint32_t src = src_base + HEADER_FIRST_UNIT_OFFSET;
    uint32_t dst = dst_base + HEADER_FIRST_UNIT_OFFSET;
    do {
        uint32_t id = eeprom_read32(src);
        if (id == PAST_UNIT_ID_END) {
            break;
        }
        uint32_t size = eeprom_read32(src + UNIT_SIZE_OFFSET);
        uint32_t aligned_size = size;
        if (aligned_size % 4) {
            aligned_size += 4 - aligned_size % 4;
        }
        if (id != PAST_UNIT_ID_INVALID) {
            for (uint32_t i = 0 && success; i < aligned_size / 4; i++) {
                success &= eeprom_write32(dst + UNIT_DATA_OFFSET + 4*i, eeprom_read32(src + UNIT_DATA_OFFSET + 4*i));
            }
            success &= eeprom_write32(dst + UNIT_SIZE_OFFSET, size);
            success &= eeprom_write32(dst, id);
            if (!success) {
                break;
            }
            dst += UNIT_DATA_OFFSET + aligned_size;
        }
        src += UNIT_DATA_OFFSET + aligned_size;
    } while (src < src_base + past->_block_size);
    if (success) {
        past->_end_addr = dst;
    }
    return success;
}
#endif // CONFIG_PAST_MINIMAL

/**
 * @brief      Convenience function for writing an uint32_t to past
 *
 * @param[in]  id     the past unit
 * @param[in]  value  the value
 *
 * @return     true if writing went well
 */
bool past_write_uint32(past_t *past, past_id_t id, uint32_t value)
{
    return past_write_unit(past, id, (void*)&value, sizeof(uint32_t));
}

/**
 * @brief      Convenience function for reading an uint32_t from past
 *
 * @param[in]  id     the past unit
 * @param[in]  value  the value
 *
 * @return     true if unit existed and reading went well
 */
bool past_read_uint32(past_t *past, past_id_t id, uint32_t *value)
{
    uint32_t length;
    uint32_t *temp;
    if (past_read_unit(past, id, (const void**)&temp, &length)) {
        if (length == sizeof(uint32_t)) {
            *value = *temp;
            return true;
        }
    }
    return false;
}

/**
 * @brief      Convenience function for writing C string (including zero
 *             terminator) to past
 *
 * @param[in]  id     the past unit
 * @param[in]  str  the string
 *
 * @return     true if writing went well
 */
bool past_write_cstr(past_t *past, past_id_t id, char *str)
{
   return past_write_unit(past, id, (void*)str, strlen(str) + 1);
}
