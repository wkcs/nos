/** Copyright (C) 2026 Nick Hu */
#include <drivers/w25q_flash.h>
#include <kernel/mm.h>
#include <kernel/sleep.h>
#include <string.h>

#define W25Q_SECTOR_SIZE 4096U
#define W25Q_PAGE_SIZE 256U
#define W25Q_MAX_CAPACITY 0x1000000U
#define W25Q_POLL_LIMIT 2000U

struct w25q_wl_meta_hdr {
    uint32_t magic;
    uint32_t seq;
    uint16_t logical_blocks;
    uint16_t data_phys_sectors;
    uint32_t crc32;
} __attribute__((packed));

static int w25q_wait_ready(const struct w25q_spi *spi)
{
    uint32_t remaining = W25Q_POLL_LIMIT;
    uint8_t status;
    int ret;

    while (remaining-- != 0) {
        ret = w25q_spi_read_status(spi, &status);
        if (ret != 0) {
            return ret;
        }
        if ((status & 1U) == 0) {
            return 0;
        }
        msleep(1);
    }
    return -ETIMEDOUT;
}

static int w25q_program(const struct w25q_spi *spi, uint32_t address,
                        const uint8_t *data, size_t length)
{
    size_t chunk;
    int ret;

    while (length != 0) {
        chunk = W25Q_PAGE_SIZE - (address % W25Q_PAGE_SIZE);
        if (chunk > length) chunk = length;
        ret = w25q_spi_write_enable(spi);
        if (ret == 0) ret = w25q_spi_program_page(spi, address, data, chunk);
        if (ret == 0) ret = w25q_wait_ready(spi);
        if (ret != 0) return ret;
        address += (uint32_t)chunk;
        data += chunk;
        length -= chunk;
    }
    return 0;
}

static int w25q_erase_one_sector(const struct w25q_spi *spi, uint32_t sector_address)
{
    int ret = w25q_spi_write_enable(spi);

    if (ret == 0) {
        ret = w25q_spi_erase_sector(spi, sector_address);
    }
    if (ret == 0) {
        ret = w25q_wait_ready(spi);
    }
    return ret;
}

static int w25q_program_sparse_sector(const struct w25q_spi *spi,
                                      uint32_t sector_address,
                                      const uint8_t *sector)
{
    size_t page;
    size_t i;
    int ret = 0;

    for (page = 0; page < W25Q_SECTOR_SIZE && ret == 0; page += W25Q_PAGE_SIZE) {
        for (i = 0; i < W25Q_PAGE_SIZE && sector[page + i] == 0xffU; i++) {
        }
        if (i != W25Q_PAGE_SIZE) {
            ret = w25q_program(spi, sector_address + (uint32_t)page,
                               sector + page, W25Q_PAGE_SIZE);
        }
    }
    return ret;
}

int w25q_flash_init(struct w25q_flash *flash, const struct w25q_spi *spi,
                    uint32_t capacity)
{
    if (flash == NULL || spi == NULL || capacity == 0 ||
        capacity > W25Q_MAX_CAPACITY || capacity % W25Q_SECTOR_SIZE != 0) {
        return -EINVAL;
    }
    flash->spi = spi;
    flash->capacity = capacity;
    return mutex_init(&flash->lock);
}

int w25q_flash_read(struct w25q_flash *flash, uint32_t address,
                    void *buffer, size_t length)
{
    int ret;

    if (flash == NULL || address > flash->capacity ||
        length > flash->capacity - address || (length != 0 && buffer == NULL)) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    mutex_lock(&flash->lock);
    ret = w25q_wait_ready(flash->spi);
    if (ret == 0) {
        ret = w25q_spi_read(flash->spi, address, buffer, length);
    }
    mutex_unlock(&flash->lock);
    return ret;
}

int w25q_flash_write(struct w25q_flash *flash, uint32_t address,
                     const void *buffer, size_t length)
{
    const uint8_t *source = buffer;
    uint8_t *sector = NULL;
    size_t remaining = length;
    size_t chunk;
    size_t offset;
    size_t i;
    uint32_t sector_address;
    int ret = 0;

    if (flash == NULL || address > flash->capacity ||
        length > flash->capacity - address || (length != 0 && buffer == NULL)) {
        return -EINVAL;
    }
    if (length == 0) {
        return 0;
    }
    mutex_lock(&flash->lock);
    ret = w25q_wait_ready(flash->spi);
    while (remaining != 0 && ret == 0) {
        size_t pos = 0;
        int identical = 1;
        int erase_required = 0;

        sector_address = address & ~(W25Q_SECTOR_SIZE - 1U);
        offset = address - sector_address;
        chunk = W25Q_SECTOR_SIZE - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }
        while (pos < chunk && ret == 0) {
            size_t sub = chunk - pos;
            if (sub > sizeof(flash->page_buf)) {
                sub = sizeof(flash->page_buf);
            }
            ret = w25q_spi_read(flash->spi, address + (uint32_t)pos,
                                flash->page_buf, sub);
            if (ret != 0) {
                break;
            }
            if (memcmp(flash->page_buf, source + pos, sub) != 0) {
                identical = 0;
                for (i = 0; i < sub; i++) {
                    if ((flash->page_buf[i] & source[pos + i]) != source[pos + i]) {
                        erase_required = 1;
                        break;
                    }
                }
            }
            pos += sub;
        }
        if (ret != 0) {
            break;
        }
        if (!identical) {
            if (!erase_required) {
                ret = w25q_program(flash->spi, address, source, chunk);
            } else if (offset == 0U && chunk == W25Q_SECTOR_SIZE) {
                ret = w25q_erase_one_sector(flash->spi, sector_address);
                if (ret == 0) {
                    ret = w25q_program_sparse_sector(flash->spi, sector_address, source);
                }
            } else {
                if (sector == NULL) {
                    sector = kmalloc(W25Q_SECTOR_SIZE, GFP_KERNEL);
                    if (sector == NULL) {
                        ret = -ENOMEM;
                        break;
                    }
                }
                ret = w25q_spi_read(flash->spi, sector_address, sector, W25Q_SECTOR_SIZE);
                if (ret != 0) {
                    break;
                }
                memcpy(sector + offset, source, chunk);
                ret = w25q_erase_one_sector(flash->spi, sector_address);
                if (ret == 0) {
                    ret = w25q_program_sparse_sector(flash->spi, sector_address, sector);
                }
            }
            if (ret != 0) break;
        }
        source += chunk;
        address += (uint32_t)chunk;
        remaining -= chunk;
    }
    mutex_unlock(&flash->lock);
    if (sector != NULL) {
        kfree(sector);
    }
    return ret;
}

int w25q_flash_erase(struct w25q_flash *flash, uint32_t address,
                     size_t length)
{
    int ret = 0;

    if (flash == NULL || address > flash->capacity ||
        length > flash->capacity - address ||
        (address % W25Q_SECTOR_SIZE) != 0U ||
        (length % W25Q_SECTOR_SIZE) != 0U) {
        return -EINVAL;
    }
    if (length == 0U) {
        return 0;
    }
    mutex_lock(&flash->lock);
    ret = w25q_wait_ready(flash->spi);
    while (length != 0U && ret == 0) {
        ret = w25q_erase_one_sector(flash->spi, address);
        address += W25Q_SECTOR_SIZE;
        length -= W25Q_SECTOR_SIZE;
    }
    mutex_unlock(&flash->lock);
    return ret;
}

static uint32_t w25q_wl_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    unsigned int b;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8U; b++) {
            crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return ~crc;
}

static inline bool w25q_wl_is_used(const struct w25q_wl *wl, uint16_t phys)
{
    return (wl->used_bitmap[phys >> 3U] & (uint8_t)(1U << (phys & 7U))) != 0U;
}

static inline void w25q_wl_mark_used(struct w25q_wl *wl, uint16_t phys)
{
    wl->used_bitmap[phys >> 3U] |= (uint8_t)(1U << (phys & 7U));
}

static inline void w25q_wl_mark_free(struct w25q_wl *wl, uint16_t phys)
{
    wl->used_bitmap[phys >> 3U] &= (uint8_t)~(1U << (phys & 7U));
}

static uint16_t w25q_wl_record_size(const struct w25q_wl *wl)
{
    uint32_t raw = (uint32_t)sizeof(struct w25q_wl_meta_hdr) +
                   (uint32_t)wl->logical_blocks * sizeof(uint16_t) +
                   (uint32_t)wl->data_phys_sectors * sizeof(uint16_t);
    uint32_t aligned = (raw + 15U) & ~15U;

    return (aligned > W25Q_SECTOR_SIZE) ? (uint16_t)W25Q_SECTOR_SIZE : (uint16_t)aligned;
}

static uint32_t w25q_wl_meta_addr(const struct w25q_wl *wl, uint8_t idx)
{
    return wl->base_addr +
           ((uint32_t)wl->data_phys_sectors + (uint32_t)(idx & 1U)) * W25Q_SECTOR_SIZE;
}

static void w25q_wl_set_identity(struct w25q_wl *wl)
{
    uint16_t i;

    memset(wl->l2p, 0, sizeof(wl->l2p));
    memset(wl->erase_counts, 0, sizeof(wl->erase_counts));
    memset(wl->used_bitmap, 0, sizeof(wl->used_bitmap));
    for (i = 0; i < wl->logical_blocks; i++) {
        wl->l2p[i] = i;
        w25q_wl_mark_used(wl, i);
    }
    wl->meta_seq = 0U;
    wl->active_meta_idx = 0U;
    wl->next_meta_off = W25Q_SECTOR_SIZE;
}

static int w25q_wl_is_slot_erased_spi(struct w25q_wl *wl, uint32_t slot_addr,
                                      uint16_t rec_size, bool *erased)
{
    uint8_t *page_buf = wl->flash->page_buf;
    uint16_t pos = 0;
    uint16_t i;
    int ret;

    *erased = true;
    while (pos < rec_size) {
        uint16_t sub = (uint16_t)(rec_size - pos);
        if (sub > W25Q_PAGE_SIZE) {
            sub = W25Q_PAGE_SIZE;
        }
        ret = w25q_spi_read(wl->flash->spi, slot_addr + pos, page_buf, sub);
        if (ret != 0) {
            return ret;
        }
        for (i = 0; i < sub; i++) {
            if (page_buf[i] != 0xffU) {
                *erased = false;
                return 0;
            }
        }
        pos = (uint16_t)(pos + sub);
    }
    return 0;
}

static bool w25q_wl_validate_record_spi(struct w25q_wl *wl, uint32_t slot_addr,
                                        struct w25q_wl_meta_hdr *out_hdr)
{
    uint8_t *page_buf = wl->flash->page_buf;
    struct w25q_wl_meta_hdr hdr;
    uint8_t seen[W25Q_WL_MAX_BLOCKS / 8U];
    uint32_t l2p_addr;
    uint32_t ec_addr;
    uint32_t l2p_bytes;
    uint32_t ec_bytes;
    uint32_t pos;
    uint32_t calc_crc;

    if (w25q_spi_read(wl->flash->spi, slot_addr, &hdr, sizeof(hdr)) != 0) {
        return false;
    }
    if (hdr.magic != W25Q_WL_META_MAGIC ||
        hdr.logical_blocks != wl->logical_blocks ||
        hdr.data_phys_sectors != wl->data_phys_sectors) {
        return false;
    }

    calc_crc = w25q_wl_crc32(0U, &hdr, offsetof(struct w25q_wl_meta_hdr, crc32));
    l2p_addr = slot_addr + (uint32_t)sizeof(hdr);
    l2p_bytes = (uint32_t)wl->logical_blocks * sizeof(uint16_t);
    ec_addr = l2p_addr + l2p_bytes;
    ec_bytes = (uint32_t)wl->data_phys_sectors * sizeof(uint16_t);

    memset(seen, 0, sizeof(seen));
    for (pos = 0; pos < l2p_bytes; ) {
        uint32_t sub = l2p_bytes - pos;
        uint32_t off;
        if (sub > W25Q_PAGE_SIZE) {
            sub = W25Q_PAGE_SIZE;
        }
        if (w25q_spi_read(wl->flash->spi, l2p_addr + pos, page_buf, sub) != 0) {
            return false;
        }
        calc_crc = w25q_wl_crc32(calc_crc, page_buf, sub);
        for (off = 0; off < sub; off += sizeof(uint16_t)) {
            uint16_t phys;
            memcpy(&phys, page_buf + off, sizeof(uint16_t));
            if (phys >= wl->data_phys_sectors ||
                (seen[phys >> 3U] & (uint8_t)(1U << (phys & 7U))) != 0U) {
                return false;
            }
            seen[phys >> 3U] |= (uint8_t)(1U << (phys & 7U));
        }
        pos += sub;
    }

    for (pos = 0; pos < ec_bytes; ) {
        uint32_t sub = ec_bytes - pos;
        if (sub > W25Q_PAGE_SIZE) {
            sub = W25Q_PAGE_SIZE;
        }
        if (w25q_spi_read(wl->flash->spi, ec_addr + pos, page_buf, sub) != 0) {
            return false;
        }
        calc_crc = w25q_wl_crc32(calc_crc, page_buf, sub);
        pos += sub;
    }

    if (calc_crc != hdr.crc32) {
        return false;
    }
    *out_hdr = hdr;
    return true;
}

static int w25q_wl_load_meta(struct w25q_wl *wl)
{
    uint16_t rec_size = w25q_wl_record_size(wl);
    bool found = false;
    uint8_t idx;
    uint16_t off;

    w25q_wl_set_identity(wl);

    for (idx = 0; idx < W25Q_WL_META_SECTORS; idx++) {
        uint32_t sec_addr = w25q_wl_meta_addr(wl, idx);

        for (off = 0; (uint32_t)off + rec_size <= W25Q_SECTOR_SIZE; off += rec_size) {
            uint32_t slot_addr = sec_addr + off;
            struct w25q_wl_meta_hdr hdr;
            bool erased = false;
            int ret;

            ret = w25q_wl_is_slot_erased_spi(wl, slot_addr, rec_size, &erased);
            if (ret != 0) {
                return ret;
            }
            if (erased) {
                continue;
            }
            if (!w25q_wl_validate_record_spi(wl, slot_addr, &hdr)) {
                continue;
            }
            if (!found || (int32_t)(hdr.seq - wl->meta_seq) > 0) {
                uint32_t l2p_addr = slot_addr + (uint32_t)sizeof(hdr);
                uint32_t l2p_bytes = (uint32_t)wl->logical_blocks * sizeof(uint16_t);
                uint32_t ec_addr = l2p_addr + l2p_bytes;
                uint32_t ec_bytes = (uint32_t)wl->data_phys_sectors * sizeof(uint16_t);
                uint16_t i;

                ret = w25q_spi_read(wl->flash->spi, l2p_addr, wl->l2p, l2p_bytes);
                if (ret == 0) {
                    ret = w25q_spi_read(wl->flash->spi, ec_addr,
                                        wl->erase_counts, ec_bytes);
                }
                if (ret != 0) {
                    return ret;
                }
                found = true;
                wl->meta_seq = hdr.seq;
                wl->active_meta_idx = idx;
                wl->next_meta_off = (uint16_t)(off + rec_size);
                memset(wl->used_bitmap, 0, sizeof(wl->used_bitmap));
                for (i = 0; i < wl->logical_blocks; i++) {
                    w25q_wl_mark_used(wl, wl->l2p[i]);
                }
            }
        }
    }

    return 0;
}

static int w25q_wl_program_meta_record(struct w25q_wl *wl, uint32_t target_addr,
                                       const struct w25q_wl_meta_hdr *hdr)
{
    uint32_t l2p_bytes = (uint32_t)wl->logical_blocks * sizeof(uint16_t);
    uint32_t ec_bytes = (uint32_t)wl->data_phys_sectors * sizeof(uint16_t);
    int ret;

    ret = w25q_program(wl->flash->spi, target_addr,
                       (const uint8_t *)hdr, sizeof(*hdr));
    if (ret == 0) {
        ret = w25q_program(wl->flash->spi, target_addr + (uint32_t)sizeof(*hdr),
                           (const uint8_t *)wl->l2p, l2p_bytes);
    }
    if (ret == 0) {
        ret = w25q_program(wl->flash->spi,
                           target_addr + (uint32_t)sizeof(*hdr) + l2p_bytes,
                           (const uint8_t *)wl->erase_counts, ec_bytes);
    }
    return ret;
}

static int w25q_wl_commit_meta_locked(struct w25q_wl *wl)
{
    uint16_t rec_size = w25q_wl_record_size(wl);
    struct w25q_wl_meta_hdr hdr;
    uint32_t target_addr;
    int ret;

    hdr.magic = W25Q_WL_META_MAGIC;
    hdr.seq = ++wl->meta_seq;
    hdr.logical_blocks = wl->logical_blocks;
    hdr.data_phys_sectors = wl->data_phys_sectors;
    hdr.crc32 = 0U;
    hdr.crc32 = w25q_wl_crc32(0U, &hdr, offsetof(struct w25q_wl_meta_hdr, crc32));
    hdr.crc32 = w25q_wl_crc32(hdr.crc32, wl->l2p,
                              (size_t)wl->logical_blocks * sizeof(uint16_t));
    hdr.crc32 = w25q_wl_crc32(hdr.crc32, wl->erase_counts,
                              (size_t)wl->data_phys_sectors * sizeof(uint16_t));

    if ((uint32_t)wl->next_meta_off + rec_size <= W25Q_SECTOR_SIZE) {
        target_addr = w25q_wl_meta_addr(wl, wl->active_meta_idx) + wl->next_meta_off;
        ret = w25q_wl_program_meta_record(wl, target_addr, &hdr);
        if (ret == 0) {
            wl->next_meta_off = (uint16_t)(wl->next_meta_off + rec_size);
        }
        return ret;
    }

    /* Active metadata sector is full: ping-pong to the alternate sector */
    wl->active_meta_idx ^= 1U;
    target_addr = w25q_wl_meta_addr(wl, wl->active_meta_idx);
    ret = w25q_erase_one_sector(wl->flash->spi, target_addr);
    if (ret == 0) {
        ret = w25q_wl_program_meta_record(wl, target_addr, &hdr);
    }
    if (ret == 0) {
        wl->next_meta_off = rec_size;
    }
    return ret;
}

int w25q_wl_init(struct w25q_wl *wl, struct w25q_flash *flash,
                 uint32_t base_addr, uint32_t region_size)
{
    uint32_t total_sectors;
    uint16_t data_sectors;
    uint16_t spares;
    int ret;

    if (wl == NULL || flash == NULL ||
        base_addr > flash->capacity ||
        region_size > flash->capacity - base_addr ||
        (base_addr % W25Q_SECTOR_SIZE) != 0U ||
        (region_size % W25Q_SECTOR_SIZE) != 0U) {
        return -EINVAL;
    }

    total_sectors = region_size / W25Q_SECTOR_SIZE;
    if (total_sectors < (W25Q_WL_META_SECTORS + 2U)) {
        return -EINVAL;
    }
    if (total_sectors > (W25Q_WL_MAX_BLOCKS + W25Q_WL_META_SECTORS)) {
        total_sectors = W25Q_WL_MAX_BLOCKS + W25Q_WL_META_SECTORS;
    }
    data_sectors = (uint16_t)(total_sectors - W25Q_WL_META_SECTORS);
    if (data_sectors >= 16U) {
        spares = 4U;
    } else if (data_sectors >= 6U) {
        spares = 2U;
    } else {
        spares = 1U;
    }

    memset(wl, 0, sizeof(*wl));
    wl->flash = flash;
    wl->base_addr = base_addr;
    wl->region_size = total_sectors * W25Q_SECTOR_SIZE;
    wl->total_phys_sectors = (uint16_t)total_sectors;
    wl->data_phys_sectors = data_sectors;
    wl->spare_sectors = spares;
    wl->logical_blocks = (uint16_t)(data_sectors - spares);

    mutex_lock(&flash->lock);
    ret = w25q_wait_ready(flash->spi);
    if (ret == 0) {
        ret = w25q_wl_load_meta(wl);
    }
    mutex_unlock(&flash->lock);

    return ret;
}

int w25q_wl_reset(struct w25q_wl *wl)
{
    int ret;

    if (wl == NULL || wl->flash == NULL) {
        return -EINVAL;
    }
    mutex_lock(&wl->flash->lock);
    ret = w25q_wait_ready(wl->flash->spi);
    if (ret == 0) {
        ret = w25q_erase_one_sector(wl->flash->spi, w25q_wl_meta_addr(wl, 0U));
    }
    if (ret == 0) {
        ret = w25q_erase_one_sector(wl->flash->spi, w25q_wl_meta_addr(wl, 1U));
    }
    if (ret == 0) {
        w25q_wl_set_identity(wl);
    }
    mutex_unlock(&wl->flash->lock);
    return ret;
}

int w25q_wl_get_info(const struct w25q_wl *wl, uint32_t *sector_size,
                     uint32_t *sector_count)
{
    if (wl == NULL || wl->flash == NULL ||
        sector_size == NULL || sector_count == NULL) {
        return -EINVAL;
    }
    *sector_size = W25Q_WL_LOGICAL_SECTOR_SIZE;
    *sector_count = (uint32_t)wl->logical_blocks * W25Q_WL_SUBSECTORS_PER_BLOCK;
    return 0;
}

int w25q_wl_read(struct w25q_wl *wl, uint32_t lba, void *buf, uint32_t count)
{
    uint8_t *dst = (uint8_t *)buf;
    uint32_t total_lbas;
    int ret = 0;

    if (wl == NULL || wl->flash == NULL || buf == NULL || count == 0U) {
        return -EINVAL;
    }
    total_lbas = (uint32_t)wl->logical_blocks * W25Q_WL_SUBSECTORS_PER_BLOCK;
    if (lba >= total_lbas || count > total_lbas - lba) {
        return -EINVAL;
    }

    mutex_lock(&wl->flash->lock);
    ret = w25q_wait_ready(wl->flash->spi);
    while (count != 0U && ret == 0) {
        uint16_t lblock = (uint16_t)(lba / W25Q_WL_SUBSECTORS_PER_BLOCK);
        uint32_t sub_idx = lba % W25Q_WL_SUBSECTORS_PER_BLOCK;
        uint32_t sub_cnt = W25Q_WL_SUBSECTORS_PER_BLOCK - sub_idx;
        uint32_t phys_addr;

        if (sub_cnt > count) {
            sub_cnt = count;
        }
        phys_addr = wl->base_addr +
                    (uint32_t)wl->l2p[lblock] * W25Q_SECTOR_SIZE +
                    sub_idx * W25Q_WL_LOGICAL_SECTOR_SIZE;
        ret = w25q_spi_read(wl->flash->spi, phys_addr, dst,
                            sub_cnt * W25Q_WL_LOGICAL_SECTOR_SIZE);
        dst += sub_cnt * W25Q_WL_LOGICAL_SECTOR_SIZE;
        lba += sub_cnt;
        count -= sub_cnt;
    }
    mutex_unlock(&wl->flash->lock);
    return ret;
}

static uint16_t w25q_wl_pick_min_spare(const struct w25q_wl *wl)
{
    uint16_t best = 0xffffU;
    uint16_t best_ec = 0xffffU;
    uint16_t p;

    for (p = 0; p < wl->data_phys_sectors; p++) {
        if (!w25q_wl_is_used(wl, p) &&
            (best == 0xffffU || wl->erase_counts[p] < best_ec)) {
            best = p;
            best_ec = wl->erase_counts[p];
        }
    }
    return best;
}

static uint16_t w25q_wl_pick_max_spare(const struct w25q_wl *wl)
{
    uint16_t best = 0xffffU;
    uint16_t best_ec = 0U;
    uint16_t p;

    for (p = 0; p < wl->data_phys_sectors; p++) {
        if (!w25q_wl_is_used(wl, p) &&
            (best == 0xffffU || wl->erase_counts[p] > best_ec)) {
            best = p;
            best_ec = wl->erase_counts[p];
        }
    }
    return best;
}

/*
 * Static wear leveling: if the erase-count spread between the hottest physical
 * sector and the coldest allocated data block reaches W25Q_WL_STATIC_THRESHOLD,
 * migrate the cold logical block onto the highest-erase spare sector so the
 * cold physical sector enters the spare pool for upcoming hot writes.
 */
static int w25q_wl_copy_sector_with_patch(struct w25q_wl *wl,
                                          uint32_t old_addr,
                                          uint32_t new_addr,
                                          uint32_t patch_off,
                                          const uint8_t *patch_buf,
                                          uint32_t patch_len)
{
    uint8_t *page_buf = wl->flash->page_buf;
    uint32_t page;
    int ret = 0;

    for (page = 0; page < W25Q_SECTOR_SIZE && ret == 0; page += W25Q_PAGE_SIZE) {
        const uint8_t *src_page;
        size_t i;

        if (patch_buf != NULL &&
            page >= patch_off && page + W25Q_PAGE_SIZE <= patch_off + patch_len) {
            src_page = patch_buf + (page - patch_off);
        } else {
            ret = w25q_spi_read(wl->flash->spi, old_addr + page,
                                page_buf, W25Q_PAGE_SIZE);
            if (ret != 0) {
                break;
            }
            if (patch_buf != NULL &&
                page < patch_off + patch_len && page + W25Q_PAGE_SIZE > patch_off) {
                uint32_t start = (patch_off > page) ? (patch_off - page) : 0U;
                uint32_t end = (patch_off + patch_len < page + W25Q_PAGE_SIZE) ?
                               (patch_off + patch_len - page) : W25Q_PAGE_SIZE;
                memcpy(page_buf + start, patch_buf + (page + start - patch_off),
                       end - start);
            }
            src_page = page_buf;
        }

        for (i = 0; i < W25Q_PAGE_SIZE && src_page[i] == 0xffU; i++) {
        }
        if (i != W25Q_PAGE_SIZE) {
            ret = w25q_program(wl->flash->spi, new_addr + page,
                               src_page, W25Q_PAGE_SIZE);
        }
    }
    return ret;
}

static int w25q_wl_check_static_leveling(struct w25q_wl *wl)
{
    uint16_t max_ec = 0U;
    uint16_t min_used_ec = 0xffffU;
    uint16_t cold_lblock = 0xffffU;
    uint16_t cold_phys;
    uint16_t hot_spare;
    uint32_t cold_addr;
    uint32_t spare_addr;
    uint16_t i;
    int ret;

    for (i = 0; i < wl->data_phys_sectors; i++) {
        if (wl->erase_counts[i] > max_ec) {
            max_ec = wl->erase_counts[i];
        }
    }
    for (i = 0; i < wl->logical_blocks; i++) {
        uint16_t p = wl->l2p[i];
        if (wl->erase_counts[p] < min_used_ec) {
            min_used_ec = wl->erase_counts[p];
            cold_lblock = i;
        }
    }
    if (cold_lblock == 0xffffU ||
        (uint32_t)(max_ec - min_used_ec) < W25Q_WL_STATIC_THRESHOLD) {
        return 0;
    }

    hot_spare = w25q_wl_pick_max_spare(wl);
    cold_phys = wl->l2p[cold_lblock];
    if (hot_spare == 0xffffU || wl->erase_counts[hot_spare] <= min_used_ec) {
        return 0;
    }

    cold_addr = wl->base_addr + (uint32_t)cold_phys * W25Q_SECTOR_SIZE;
    spare_addr = wl->base_addr + (uint32_t)hot_spare * W25Q_SECTOR_SIZE;
    ret = w25q_erase_one_sector(wl->flash->spi, spare_addr);
    if (ret != 0) {
        return ret;
    }
    if (wl->erase_counts[hot_spare] < 0xffffU) {
        wl->erase_counts[hot_spare]++;
    }
    wl->total_erases++;

    ret = w25q_wl_copy_sector_with_patch(wl, cold_addr, spare_addr, 0U, NULL, 0U);
    if (ret != 0) {
        return ret;
    }

    wl->l2p[cold_lblock] = hot_spare;
    w25q_wl_mark_used(wl, hot_spare);
    w25q_wl_mark_free(wl, cold_phys);
    wl->static_migrations++;
    return 0;
}

int w25q_wl_write(struct w25q_wl *wl, uint32_t lba, const void *buf,
                  uint32_t count)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t total_lbas;
    bool meta_dirty = false;
    int ret = 0;

    if (wl == NULL || wl->flash == NULL || buf == NULL || count == 0U) {
        return -EINVAL;
    }
    total_lbas = (uint32_t)wl->logical_blocks * W25Q_WL_SUBSECTORS_PER_BLOCK;
    if (lba >= total_lbas || count > total_lbas - lba) {
        return -EINVAL;
    }

    mutex_lock(&wl->flash->lock);
    ret = w25q_wait_ready(wl->flash->spi);
    while (count != 0U && ret == 0) {
        uint16_t lblock = (uint16_t)(lba / W25Q_WL_SUBSECTORS_PER_BLOCK);
        uint32_t sub_idx = lba % W25Q_WL_SUBSECTORS_PER_BLOCK;
        uint32_t sub_cnt = W25Q_WL_SUBSECTORS_PER_BLOCK - sub_idx;
        uint32_t sub_off = sub_idx * W25Q_WL_LOGICAL_SECTOR_SIZE;
        uint32_t sub_len;
        uint16_t old_phys = wl->l2p[lblock];
        uint32_t old_addr = wl->base_addr + (uint32_t)old_phys * W25Q_SECTOR_SIZE;
        uint32_t pos = 0U;
        bool identical = true;
        bool need_erase = false;

        if (sub_cnt > count) {
            sub_cnt = count;
        }
        sub_len = sub_cnt * W25Q_WL_LOGICAL_SECTOR_SIZE;

        while (pos < sub_len && ret == 0) {
            uint32_t page_chunk = sub_len - pos;
            size_t i;

            if (page_chunk > W25Q_PAGE_SIZE) {
                page_chunk = W25Q_PAGE_SIZE;
            }
            ret = w25q_spi_read(wl->flash->spi, old_addr + sub_off + pos,
                                wl->flash->page_buf, page_chunk);
            if (ret != 0) {
                break;
            }
            if (memcmp(wl->flash->page_buf, src + pos, page_chunk) != 0) {
                identical = false;
                for (i = 0; i < page_chunk; i++) {
                    if ((wl->flash->page_buf[i] & src[pos + i]) != src[pos + i]) {
                        need_erase = true;
                        break;
                    }
                }
            }
            pos += page_chunk;
        }
        if (ret != 0) {
            break;
        }

        if (!identical) {
            if (!need_erase) {
                ret = w25q_program(wl->flash->spi, old_addr + sub_off, src, sub_len);
            } else {
                uint16_t new_phys = w25q_wl_pick_min_spare(wl);
                uint32_t new_addr;

                if (new_phys == 0xffffU) {
                    new_phys = old_phys;
                }
                new_addr = wl->base_addr + (uint32_t)new_phys * W25Q_SECTOR_SIZE;

                ret = w25q_erase_one_sector(wl->flash->spi, new_addr);
                if (ret == 0) {
                    if (wl->erase_counts[new_phys] < 0xffffU) {
                        wl->erase_counts[new_phys]++;
                    }
                    wl->total_erases++;
                    ret = w25q_wl_copy_sector_with_patch(wl, old_addr, new_addr,
                                                         sub_off, src, sub_len);
                }
                if (ret == 0) {
                    wl->l2p[lblock] = new_phys;
                    w25q_wl_mark_used(wl, new_phys);
                    if (new_phys != old_phys) {
                        w25q_wl_mark_free(wl, old_phys);
                    }
                    meta_dirty = true;
                    ret = w25q_wl_check_static_leveling(wl);
                }
            }
            if (ret != 0) {
                break;
            }
        }

        src += sub_len;
        lba += sub_cnt;
        count -= sub_cnt;
    }

    if (ret == 0 && meta_dirty) {
        ret = w25q_wl_commit_meta_locked(wl);
    }
    mutex_unlock(&wl->flash->lock);
    return ret;
}

static int w25q_wl_block_read(const struct device *dev, uint32_t lba,
                              void *buf, uint32_t count)
{
    if (dev == NULL || dev->data == NULL) {
        return -EINVAL;
    }
    return w25q_wl_read((struct w25q_wl *)dev->data, lba, buf, count);
}

static int w25q_wl_block_write(const struct device *dev, uint32_t lba,
                               const void *buf, uint32_t count)
{
    if (dev == NULL || dev->data == NULL) {
        return -EINVAL;
    }
    return w25q_wl_write((struct w25q_wl *)dev->data, lba, buf, count);
}

static int w25q_wl_block_get_info(const struct device *dev,
                                  uint32_t *sector_size,
                                  uint32_t *sector_count)
{
    if (dev == NULL || dev->data == NULL) {
        return -EINVAL;
    }
    return w25q_wl_get_info((const struct w25q_wl *)dev->data,
                            sector_size, sector_count);
}

const struct block_driver_api w25q_wl_block_driver_api = {
    .read = w25q_wl_block_read,
    .write = w25q_wl_block_write,
    .get_info = w25q_wl_block_get_info,
};
