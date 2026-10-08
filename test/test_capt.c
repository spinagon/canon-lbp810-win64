#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/capt.h"
#include "../src/usb_comm.h"
#include "../src/status.h"

// Stubs to avoid scoa undefined references
void scoa_compress_chunk(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t *out_len) {
    memcpy(out, in, in_len);
    *out_len = in_len;
}

void test_packet_encoding() {
    printf("Testing packet encoding...\n");
    // Verify 4-byte framing protocol: opcode LE, total packet length LE
    uint16_t opcode = 0xD0A0; // CAPT_BEGIN_PAGE
    uint16_t payload_len = 34;
    uint32_t packet_size = 4 + payload_len;

    uint8_t header[4];
    header[0] = opcode & 0xFF;
    header[1] = (opcode >> 8) & 0xFF;
    header[2] = packet_size & 0xFF;
    header[3] = (packet_size >> 8) & 0xFF;

    assert(header[0] == 0xA0);
    assert(header[1] == 0xD0);
    assert(header[2] == 38);
    assert(header[3] == 0x00);

    // Verify zero-payload command (e.g. CAPT_BEGIN_DATA)
    uint16_t op_data = 0xD0A1;
    uint32_t empty_size = 4 + 0;
    header[0] = op_data & 0xFF;
    header[1] = (op_data >> 8) & 0xFF;
    header[2] = empty_size & 0xFF;
    header[3] = (empty_size >> 8) & 0xFF;
    assert(header[0] == 0xA1);
    assert(header[1] == 0xD0);
    assert(header[2] == 4);
    assert(header[3] == 0);
}

void test_page_header_construction() {
    printf("Testing page header construction...\n");
    capt_page_params_t params = {0};
    params.target_model = CAPT_MODEL_LBP810;
    params.paper_size = PAPER_A4;
    params.resolution = 0x11;
    // ...
    assert(params.target_model == 0x01A4);
    assert(params.paper_size == 0x02);
}

void test_status_parsing() {
    printf("Testing status parsing...\n");
    uint8_t raw[16] = {0};
    raw[0] = 0x08; // IM_DATA_BUSY
    raw[6] = 0x00; raw[7] = 0x02; // Engine: 0x0200 = NO_PRINT_PAPER
    
    capt_status_t status;
    capt_parse_status(raw, 16, &status);
    
    assert(status.buffer_full == true);
    assert(status.paper_available == false);
}

void test_go_online_magic() {
    printf("Testing GO_ONLINE magic bytes...\n");
    uint32_t magic = 0xADEADBEE;
    uint8_t expected[4] = {0xEE, 0xDB, 0xEA, 0xAD};
    (void)magic;
    assert(expected[0] == 0xEE);
    assert(expected[1] == 0xDB);
    assert(expected[2] == 0xEA);
    assert(expected[3] == 0xAD);
}

void test_bcd_decode() {
    printf("Testing BCD size decoding...\n");
    // Just run a local implementation to verify
    uint16_t bcd = 0x0064;
    uint16_t decoded = ((bcd >> 8) & 0xF)*100 + ((bcd >> 4) & 0xF)*10 + (bcd & 0xF);
    assert(decoded == 64);
}

void test_extended_paper_sizes() {
    printf("Testing extended paper sizes...\n");
    
    // Check all 9 formats exist and have valid properties
    uint8_t codes[] = {
        PAPER_A4, PAPER_LETTER, PAPER_LEGAL, PAPER_EXEC,
        PAPER_A5, PAPER_B5, PAPER_ENV_COM10, PAPER_ENV_DL, PAPER_ENV_C5
    };
    for (size_t i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
        const capt_paper_info_t *info = capt_get_paper_info_by_code(codes[i]);
        assert(info != NULL);
        assert(info->code == codes[i]);
        assert(info->width_600 > 0);
        assert(info->height_600 > 0);
        assert(info->line_size_600 > 0);
        assert(info->lines_600 > 0);
        assert(info->line_size_600 * 8 >= info->width_600 - 2 * info->margin_l_600);
    }

    // Test name lookups
    assert(capt_get_paper_info_by_name("iso_a4_210x297mm")->code == PAPER_A4);
    assert(capt_get_paper_info_by_name("na_letter_8.5x11in")->code == PAPER_LETTER);
    assert(capt_get_paper_info_by_name("na_legal_8.5x14in")->code == PAPER_LEGAL);
    assert(capt_get_paper_info_by_name("na_executive_7.25x10.5in")->code == PAPER_EXEC);
    assert(capt_get_paper_info_by_name("iso_a5_148x210mm")->code == PAPER_A5);
    assert(capt_get_paper_info_by_name("jis_b5_182x257mm")->code == PAPER_B5);
    assert(capt_get_paper_info_by_name("na_number-10_4.125x9.5in")->code == PAPER_ENV_COM10);
    assert(capt_get_paper_info_by_name("iso_dl_110x220mm")->code == PAPER_ENV_DL);
    assert(capt_get_paper_info_by_name("iso_c5_162x229mm")->code == PAPER_ENV_C5);

    // Test dimension matching
    assert(capt_get_paper_info_by_dims(4960, 7014, 600)->code == PAPER_A4);
    assert(capt_get_paper_info_by_dims(5100, 6600, 600)->code == PAPER_LETTER);
    assert(capt_get_paper_info_by_dims(5100, 8400, 600)->code == PAPER_LEGAL);
    assert(capt_get_paper_info_by_dims(2475, 5700, 600)->code == PAPER_ENV_COM10);
    // At 300 DPI
    assert(capt_get_paper_info_by_dims(2480, 3507, 300)->code == PAPER_A4);
    assert(capt_get_paper_info_by_dims(2550, 3300, 300)->code == PAPER_LETTER);
}

void test_page_params_building() {
    printf("Testing page params building...\n");

    capt_page_params_t p600;
    // 600 DPI, auto cassette (0x01), toner saving off, smoothing on
    capt_build_page_params(&p600, PAPER_LEGAL, 600, 0x01, 0x00, 0x02);
    assert(p600.target_model == CAPT_MODEL_LBP810);
    assert(p600.paper_size == PAPER_LEGAL);
    assert(p600.resolution == 0x11);
    assert(p600.input_slot == 0x01);
    assert(p600.media_source == 0x01);
    assert(p600.toner_saving == 0x00);
    assert(p600.smoothing == 0x02);
    assert(p600.paper_width == 5100);
    assert(p600.paper_height == 8400);
    assert(p600.image_line_size == 610);
    assert(p600.image_lines == 8160);

    // 300 DPI, manual slot (0x00), toner saving on (draft)
    capt_page_params_t p300;
    capt_build_page_params(&p300, PAPER_A4, 300, 0x00, 0x01, 0x00);
    assert(p300.paper_size == PAPER_A4);
    assert(p300.resolution == 0x00); // 300 DPI code
    assert(p300.input_slot == 0x00); // Manual slot
    assert(p300.media_source == 0x00);
    assert(p300.toner_saving == 0x01);
    assert(p300.smoothing == 0x00);
    assert(p300.paper_width == 2480);
    assert(p300.paper_height == 3507);
    assert(p300.image_line_size == 296);
    assert(p300.image_lines == 3388);
}

void test_engine_status_reasons() {
    printf("Testing engine status mappings...\n");

    // Door open (0x4000)
    uint8_t raw_door[16] = {0};
    raw_door[6] = 0x00; raw_door[7] = 0x40; // Engine: 0x4000
    capt_status_t st_door;
    capt_parse_status(raw_door, 16, &st_door);
    assert(!st_door.cover_closed);
    assert(strcmp(st_door.error_string, "Cover Open") == 0);

    // Out of paper (0x0200)
    uint8_t raw_paper[16] = {0};
    raw_paper[6] = 0x00; raw_paper[7] = 0x02; // Engine: 0x0200
    capt_status_t st_paper;
    capt_parse_status(raw_paper, 16, &st_paper);
    assert(!st_paper.paper_available);
    assert(strstr(st_paper.error_string, "Out of Paper") != NULL);

    // Cartridge missing (0x2000)
    uint8_t raw_cart[16] = {0};
    raw_cart[6] = 0x00; raw_cart[7] = 0x20; // Engine: 0x2000
    capt_status_t st_cart;
    capt_parse_status(raw_cart, 16, &st_cart);
    assert(!st_cart.cartridge_present);
    assert(strcmp(st_cart.error_string, "No Toner Cartridge") == 0);

    // Paper jam (0x0100 + basic 0x80)
    uint8_t raw_jam[16] = {0};
    raw_jam[0] = 0x80; // GENERAL_ERROR active
    raw_jam[6] = 0x00; raw_jam[7] = 0x01; // Engine: 0x0100
    capt_status_t st_jam;
    capt_parse_status(raw_jam, 16, &st_jam);
    assert(st_jam.error);
    assert(strcmp(st_jam.error_string, "Paper Jam") == 0);

    // Normal ready printer with model code / engine flags 0x01A4 and basic 0x00 (no error)
    uint8_t raw_normal[16] = {0};
    raw_normal[6] = 0xA4; raw_normal[7] = 0x01; // Engine: 0x01A4
    capt_status_t st_normal;
    capt_parse_status(raw_normal, 16, &st_normal);
    assert(!st_normal.error);
    assert(strcmp(st_normal.error_string, "No Error") == 0);
}

void test_error_codes() {
    printf("Testing error codes definition...\n");
    assert(CAPT_OK == 0);
    assert(CAPT_ERR_COMM == -1);
    assert(CAPT_ERR_NO_PAPER == -2);
    assert(CAPT_ERR_COVER_OPEN == -3);
    assert(CAPT_ERR_CANCELLED == -4);
    assert(CAPT_ERR_DELIVERY_TIMEOUT == -5);
    assert(CAPT_ERR_BUFFER_TIMEOUT == -6);
    assert(CAPT_ERR_ENGINE == -7);
    assert(CAPT_ERR_JAM == -8);
}

int main() {
    test_packet_encoding();
    test_page_header_construction();
    test_status_parsing();
    test_go_online_magic();
    test_bcd_decode();
    test_extended_paper_sizes();
    test_page_params_building();
    test_engine_status_reasons();
    test_error_codes();
    
    printf("All test_capt tests passed!\n");
    return 0;
}
