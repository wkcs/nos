/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 * Interoperability: production CLI client against production Bootloader C engine.
 */
#include <cassert>
#include <cstring>
#include <iostream>
#include "nlk/client.hpp"
#include "nlk/caps/system.hpp"
#include "nlk/caps/flash.hpp"
#include "nlk/crc.hpp"
extern "C" {
#include "nlk_bl.h"
void test_flash_init(void);
}
class BootloaderTransport : public nlk::ITransport {
    std::vector<uint8_t> reply;
    bool drop_chunk = true;
    bool drop_commit = true;
public:
    std::string name() const override { return "actual Bootloader engine"; }
    void close() override { }
    void send_packet(const std::vector<uint8_t>& packet, int) override {
        uint8_t tx[BL_NLK_MTU];
        auto len = nlk_bl_handle_packet(packet.data(), packet.size(), tx);
        reply.assign(tx, tx + len);
        auto request = nlk::unpack_nlk_packet(packet.data(), packet.size());
        if (request.dst_cap == 1 && request.cmd_op == nlk::NLK_FLASH_OP_WRITE_CHUNK && drop_chunk) {
            reply.clear(); drop_chunk = false;
        }
        if (request.dst_cap == 1 && request.cmd_op == nlk::NLK_FLASH_OP_COMMIT && drop_commit) {
            reply.clear(); drop_commit = false;
        }
    }
    bool recv_packet(nlk::NlkPacket& packet, int) override {
        if (reply.empty()) return false;
        packet = nlk::unpack_nlk_packet(reply.data(), reply.size());
        reply.clear();
        return true;
    }
};
int main()
{
    test_flash_init();
    nlk_bl_init();
    BootloaderTransport transport;
    nlk::NLKClient client(transport);
    nlk::caps::SystemCap sys(client, 1);
    assert(sys.ping() >= 0);
    assert(std::strcmp(sys.get_device_desc().name, "NK60-V2-BL") == 0);
    auto caps = sys.get_cap_list();
    assert(caps.size() == 1 && caps[0].cap_type == nlk::NLK_CAP_FLASH);
    assert(sys.get_topology().size() == 1);
    nlk::caps::FlashCap flash(client, 1);
    auto q = flash.query_partitions();
    assert(q.run_mode == nlk::BOOT_BOOTLOADER);
    assert((q.parts[0].flags & nlk::NLK_FLASH_PART_F_WRITABLE) == 0);
    std::vector<uint8_t> image(1027, 0x5a);
    uint32_t vectors[] = {0x20010000, 0x08004009};
    std::memcpy(image.data(), vectors, sizeof(vectors));
    size_t acknowledged = 0;
    auto crc = flash.flash_partition("kernel", image, nullptr, false, [&](size_t written, size_t total) {
        assert(written >= acknowledged && written <= total);
        acknowledged = written;
    });
    assert(acknowledged == image.size());
    assert(crc == nlk::crc32(image.data(), image.size(), 0));
    assert(std::memcmp(reinterpret_cast<const void *>(0x08004000), image.data(), image.size()) == 0);
    sys.reboot(nlk::BOOT_BOOTLOADER);
    uint32_t flag = 99;
    assert(nlk_bl_is_reboot_pending(&flag) && flag == nlk::BOOT_BOOTLOADER);
    std::cout << "CLI info/flash/reboot passed against real Bootloader, including lost chunk and commit ACKs\n";
}
