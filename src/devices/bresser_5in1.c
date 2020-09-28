/** @file
    Decoder for Bresser Weather Center 5-in-1.

    Copyright (C) 2018 Daniel Krueger
    Copyright (C) 2019 Christian W. Zuckschwerdt <zany@triq.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "decoder.h"

/**
Decoder for Bresser Weather Center 7-in-1, outdoor sensor.

See https://github.com/merbanan/rtl_433/issues/1492

Also Bresser Explore Scientific SM60020 Soil moisture Sensor.
https://www.bresser.de/en/Weather-Time/Accessories/EXPLORE-SCIENTIFIC-Soil-Moisture-and-Soil-Temperature-Sensor.html

Preamble:

    aa aa aa aa aa 2d d4

Observed length depends on reset_limit.

Outdoor sensor:

    {271}631d05c09e9a18abaabaaaaaaaaa8adacbacff9cafcaaaaaaa000000000000000000

- Data whitening of 0xaa

    DIGEST:8h8h ID?8h8h WDIR:8h4h° 4h 8h WGUST:8h.4h WAVG:8h.4h RAIN:8h8h4h.4h RAIN?:8h TEMP:8h.4hC 4h HUM:8h% LIGHT:8h4h,4hKL ?:8h8h4h TRAILER:8h8h8h4h
    Unit of light is kLux (not W/m²).

First two bytes are an LFSR-16 digest, generator 0x8810 with some unknown/variable key?

Moisture:

    f16e 187000e34 7 ffffff0000 252 2 16 fff 004 000 [25,2, 99%, CH 7]
    DIGEST:8h8h ID?8h8h8h8h FLAGS:4h BATT:1b CH:3d 8h 8h8h 8h8h TEMP:12h 4h MOIST:8h TRAILER:8h8h8h8h4h

*/

static int bresser_7in1_decode(r_device *decoder, bitbuffer_t *bitbuffer)
{
    uint8_t const preamble_pattern[] = {0xaa, 0xaa, 0xaa, 0x2d, 0xd4};

    int const moisture[] = {0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 99}; // scale is 20/3

    data_t *data;
    uint8_t msg[25];

    bitbuffer_print(bitbuffer);

    if (bitbuffer->num_rows != 1 || bitbuffer->bits_per_row[0] < 240-80) {
        if (decoder->verbose > 1)
            fprintf(stderr, "%s: to few bits (%u)\n", __func__, bitbuffer->bits_per_row[0]);
        return DECODE_ABORT_LENGTH; // unrecognized
    }

    unsigned start_pos = bitbuffer_search(bitbuffer, 0, 0,
            preamble_pattern, sizeof(preamble_pattern) * 8);
    start_pos += sizeof(preamble_pattern) * 8;

    if (start_pos >= bitbuffer->bits_per_row[0]) {
        if (decoder->verbose > 1)
            fprintf(stderr, "%s: preamble not found\n", __func__);
        return DECODE_ABORT_EARLY; // no preamble found
    }
    //if (start_pos + sizeof (msg) * 8 >= bitbuffer->bits_per_row[0]) {
    if (start_pos + 21*8 >= bitbuffer->bits_per_row[0]) {
        if (decoder->verbose > 1)
            fprintf(stderr, "%s: message too short (%u)\n", __func__, bitbuffer->bits_per_row[0] - start_pos);
        return DECODE_ABORT_LENGTH; // message too short
    }

    bitbuffer_extract_bytes(bitbuffer, 0, start_pos, msg, sizeof (msg) * 8);
    bitrow_printf(msg, sizeof (msg) * 8, "MSG: ");

    if (msg[21] == 0x00) {
        return DECODE_FAIL_SANITY;
    }
    // data whitening
    for (unsigned i = 0; i < sizeof (msg); ++i) {
        msg[i] ^= 0xaa;
    }
    bitrow_printf(msg, sizeof (msg) * 8, "XOR: ");

    // LFSR-16 digest, generator 0x8810 key 0xba95 final xor 0x6df1
    int chk    = (msg[0] << 8) | msg[1];
    int digest = lfsr_digest16(&msg[2], 23, 0x8810, 0xba95);
    if ((chk ^ digest) != 0x6df1) {
        //if (decoder->verbose > 1) {
            fprintf(stderr, "%s: Digest check failed %04x vs %04x (%04x)\n", __func__, chk, digest, chk ^ digest);
        //}
        //return DECODE_FAIL_MIC;
    }

    int id       = (msg[2] << 8) | (msg[3]);
    int wdir     = (msg[4] >> 4) * 100 + (msg[4] & 0x0f) * 10 + (msg[5] >> 4);
    int wgst_raw = (msg[7] >> 4) * 100 + (msg[7] & 0x0f) * 10 + (msg[8] >> 4);
    int wavg_raw = (msg[8] & 0x0f) * 100 + (msg[9] >> 4) * 10 + (msg[9] & 0x0f);
    int rain_raw = (msg[10] >> 4) * 100000 + (msg[10] & 0x0f) * 10000 + (msg[11] >> 4) * 1000;
            + (msg[11] & 0x0f) * 100 + (msg[12] >> 4) * 10 + (msg[12] & 0x0f) * 1; // 6 digits
    float rain_mm = rain_raw * 0.1f;
    int temp_raw = (msg[14] >> 4) * 100 + (msg[14] & 0x0f) * 10 + (msg[15] >> 4);
    float temp_c = temp_raw * 0.1f;
    if (temp_raw > 600)
        temp_c = (temp_raw - 1000) * 0.1f;
    int humidity = (msg[16] >> 4) * 10 + (msg[16] & 0x0f);
    int lght_raw = (msg[17] >> 4) * 1000 + (msg[17] & 0x0f) * 100 + (msg[18] >> 4) * 10 + (msg[18] & 0x0f);

    float light_klx = lght_raw * 0.1f;

    /* clang-format off */
    data = data_make(
            "model",            "",             DATA_STRING, "Bresser-7in1",
            "id",               "",             DATA_INT,    id,
            "temperature_C",    "Temperature",  DATA_FORMAT, "%.1f C", DATA_DOUBLE, temp_c,
            "humidity",         "Humidity",     DATA_INT,    humidity,
            "wind_max_m_s",     "Wind Gust",    DATA_FORMAT, "%.1f m/s", DATA_DOUBLE, wgst_raw * 0.1f,
            "wind_avg_m_s",     "Wind Speed",   DATA_FORMAT, "%.1f m/s", DATA_DOUBLE, wavg_raw * 0.1f,
            "wind_dir_deg",     "Direction",    DATA_INT,    wdir,
            "rain_mm",          "Rain",         DATA_FORMAT, "%.1f mm", DATA_DOUBLE, rain_mm,
            "light_klx",        "Light",        DATA_FORMAT, "%.1f klx", DATA_DOUBLE, light_klx,
            "mic",              "Integrity",    DATA_STRING, "CRC",
            NULL);
    /* clang-format on */

    decoder_output_data(decoder, data);
    return 1;
}

/**
Decoder for Bresser Weather Center 6-in-1.

Also Bresser Weather Center 7-in-1 indoor sensor.

{206}55555555545ba83e803100058631ff11fe6611ffffffff01cc00 [Hum 96% Temp 3.8 C Wind 0.7 m/s]
{205}55555555545ba999263100058631fffffe66d006092bffe0cff8 [Hum 95% Temp 3.0 C Wind 0.0 m/s]
{199}55555555545ba840523100058631ff77fe668000495fff0bbe [Hum 95% Temp 3.0 C Wind 0.4 m/s]
{205}55555555545ba94d063100058631fffffe665006092bffe14ff8
{206}55555555545ba860703100058631fffffe6651ffffffff0135fc [Hum 95% Temp 3.0 C Wind 0.0 m/s]
{205}55555555545ba924d23100058631ff99fe68b004e92dffe073f8 [Hum 96% Temp 2.7 C Wind 0.4 m/s]
{202}55555555545ba813403100058631ff77fe6810050929ffe1180 [Hum 94% Temp 2.8 C Wind 0.4 m/s]
{205}55555555545ba98be83100058631fffffe6130050929ffe17800 [Hum 95% Temp 2.8 C Wind 0.8 m/s]

                                          TEMP  HUM
2dd4  1f 40 18 80 02 c3 18 ff 88 ff 33 08 ff ff ff ff 80 e6 00 [Hum 96% Temp 3.8 C Wind 0.7 m/s]
2dd4  cc 93 18 80 02 c3 18 ff ff ff 33 68 03 04 95 ff f0 67 3f [Hum 95% Temp 3.0 C Wind 0.0 m/s]
2dd4  20 29 18 80 02 c3 18 ff bb ff 33 40 00 24 af ff 85 df    [Hum 95% Temp 3.0 C Wind 0.4 m/s]
2dd4  a6 83 18 80 02 c3 18 ff ff ff 33 28 03 04 95 ff f0 a7 3f
2dd4  30 38 18 80 02 c3 18 ff ff ff 33 28 ff ff ff ff 80 9a 7f [Hum 95% Temp 3.0 C Wind 0.0 m/s]
2dd4  92 69 18 80 02 c3 18 ff cc ff 34 58 02 74 96 ff f0 39 3f [Hum 96% Temp 2.7 C Wind 0.4 m/s]
2dd4  09 a0 18 80 02 c3 18 ff bb ff 34 08 02 84 94 ff f0 8c 0  [Hum 94% Temp 2.8 C Wind 0.4 m/s]
2dd4  c5 f4 18 80 02 c3 18 ff ff ff 30 98 02 84 94 ff f0 bc 00 [Hum 95% Temp 2.8 C Wind 0.8 m/s]


{147} 5e aa 18 80 02 c3 18 fa 8f fb 27 68 11 84 81 ff f0 72 00  Temp 11.8 C  Hum 81%
{149} ae d1 18 80 02 c3 18 fa 8d fb 26 78 ff ff ff fe 02 db f0
{150} f8 2e 18 80 02 c3 18 fc c6 fd 26 38 11 84 81 ff f0 68 00  Temp 11.8 C  Hum 81%
{149} c4 7d 18 80 02 c3 18 fc 78 fd 29 28 ff ff ff fe 03 97 f0
{149} 28 1e 18 80 02 c3 18 fb b7 fc 26 58 ff ff ff fe 02 c3 f0
{150} 21 e8 18 80 02 c3 18 fb 9c fc 33 08 11 84 81 ff f0 b7 f8  Temp 11.8 C  Hum 81%
{149} 83 ae 18 80 02 c3 18 fc 78 fc 29 28 ff ff ff fe 03 98 00
{150} 5c e4 18 80 02 c3 18 fb ba fc 26 98 11 84 81 ff f0 16 00  Temp 11.8 C  Hum 81%
{148} d0 bd 18 80 02 c3 18 f9 ad fa 26 48 ff ff ff fe 02 ff f0

    DIGEST:8h8h ID?8h8h8h8h FLAGS:4h BATT:1b CH:3d WSPEED:~8h~4h ~4h~8h WDIR:12h ?4h TEMP8h.4h ?4h HUM8h UV?~12h ?4h CHKSUM:8h

Digest is LFSR-16 gen 0x8810 key 0x5412, excluding the add-checksum and trailer.
Checksum is 8-bit add (with carry) to 0xff.
*/

static int bresser_6in1_decode(r_device *decoder, bitbuffer_t *bitbuffer)
{
    uint8_t const preamble_pattern[] = {0xaa, 0xaa, 0x2d, 0xd4};

    data_t *data;
    uint8_t msg[18];

    if (bitbuffer->num_rows != 1
            || bitbuffer->bits_per_row[0] < 160
            || bitbuffer->bits_per_row[0] > 440) {
        if (decoder->verbose > 1) {
            fprintf(stderr, "%s: bit_per_row %u out of range\n", __func__, bitbuffer->bits_per_row[0]);
        }
        return DECODE_ABORT_EARLY; // Unrecognized data
    }

    unsigned start_pos = bitbuffer_search(bitbuffer, 0, 0,
            preamble_pattern, sizeof (preamble_pattern) * 8);

    if (start_pos >= bitbuffer->bits_per_row[0]) {
        return DECODE_ABORT_LENGTH;
    }
    start_pos += sizeof (preamble_pattern) * 8;

    unsigned len = bitbuffer->bits_per_row[0] - start_pos;
    if (len < sizeof(msg) * 8) {
        if (decoder->verbose > 1) {
            fprintf(stderr, "%s: %u too short\n", __func__, len);
        }
        return DECODE_ABORT_LENGTH; // message too short
    }

    bitbuffer_extract_bytes(bitbuffer, 0, start_pos, msg, sizeof(msg) * 8);

    bitrow_printf(msg, sizeof(msg) * 8, "%s: ", __func__);

    // LFSR-16 digest, generator 0x8810 init 0x5412
    int chkdgst = (msg[0] << 8) | msg[1];
    int digest  = lfsr_digest16(&msg[2], 15, 0x8810, 0x5412);
    if (chkdgst != digest) {
        //if (decoder->verbose > 1) {
        fprintf(stderr, "%s: Digest check failed %04x vs %04x\n", __func__, chkdgst, digest);
        //}
        return DECODE_FAIL_MIC;
    }
    // Checksum, add with carry
    int chksum = msg[17];
    int sum    = add_bytes(&msg[2], 16); // msg[2] to msg[17]
    if ((sum & 0xff) != 0xff) {
        if (decoder->verbose > 1) {
            fprintf(stderr, "%s: Checksum failed %04x vs %04x\n", __func__, chksum, sum);
        }
        return DECODE_FAIL_MIC;
    }

    uint32_t id  = ((uint32_t)msg[2] << 24) | (msg[3] << 16) | (msg[4] << 8) | (msg[5]);
    int flags    = (msg[6] >> 4);
    int batt     = (msg[6] >> 3) & 1;
    int chan     = (msg[6] & 0x7);

    int temp_ok = msg[12] != 0xff;
    // temperature, humidity, only if msg[12] != 0xff
    int temp_raw = (msg[12] >> 4) * 100 + (msg[12] & 0x0f) * 10 + (msg[13] >> 4);
    float temp_c = temp_raw * 0.1f;
    if (temp_raw > 600)
        temp_c = (temp_raw - 1000) * 0.1f;

    int humidity_ok = msg[14] != 0xff;
    int humidity    = (msg[14] >> 4) * 10 + (msg[14] & 0x0f);

    //int uv_ok = (msg[16] & 0x0f) == 0;
    int uv_ok  = (msg[16] & 0xf0) != 0xf0;
    int uv_raw = ((msg[15] & 0xf0) >> 4) * 100 + (msg[15] & 0x0f) * 10 + ((msg[16] & 0xf0) >> 4);
    float uv   = uv_raw * 0.1f;

    int unk_ok  = (msg[16] & 0xf0) == 0xf0;
    int unk_raw = ((msg[15] & 0xf0) >> 4) * 10 + (msg[15] & 0x0f);

    // invert 3 bytes wind speeds
    msg[7] ^= 0xff;
    msg[8] ^= 0xff;
    msg[9] ^= 0xff;
    int wind_ok = (msg[7] <= 0x99) && (msg[8] <= 0x99) && (msg[9] <= 0x99);

    int gust_raw    = (msg[7] >> 4) * 100 + (msg[7] & 0x0f) * 10 + (msg[8] >> 4);
    fprintf(stderr, "Gust %d %d %d %d\n", (msg[7] >> 4), (msg[7] & 0x0f), (msg[8] >> 4), gust_raw);
    float wind_gust = gust_raw * 0.1f;
    int wavg_raw    = (msg[9] >> 4) * 100 + (msg[9] & 0x0f) * 10 + (msg[8] & 0x0f);
    fprintf(stderr, "Avg  %d %d %d %d\n", (msg[9] >> 4), (msg[9] & 0x0f), (msg[8] & 0x0f), wavg_raw);
    float wind_avg  = wavg_raw * 0.1f;
    int wind_dir    = ((msg[10] & 0xf0) >> 4) * 100 + (msg[10] & 0x0f) * 10 + ((msg[11] & 0xf0) >> 4);

    // rain, only if msg[12] == 0xff
    // invert 2 bytes rain counter
    msg[13] ^= 0xff;
    msg[14] ^= 0xff;
    int rain_raw  = (msg[13] >> 4) * 1000 + (msg[13] & 0x0f) * 100 + (msg[14] >> 4) * 10 + (msg[14] & 0x0f);
    float rain_mm = rain_raw * 0.1f;

    /* clang-format off */
    data = data_make(
            "model",            "",             DATA_STRING, "Bresser-6in1",
            "id",               "",             DATA_INT,    id,
            "channel",          "",             DATA_INT,    chan,
            "battery_ok",       "Battery",      DATA_INT,    !batt,
            "temperature_C",    "Temperature",  DATA_COND, temp_ok, DATA_FORMAT, "%.1f C", DATA_DOUBLE, temp_c,
            "humidity",         "Humidity",     DATA_COND, humidity_ok, DATA_INT,    humidity,
            "wind_max_m_s",     "Wind Gust",    DATA_COND, wind_ok, DATA_FORMAT, "%.1f m/s", DATA_DOUBLE, wind_gust,
            "wind_avg_m_s",     "Wind Speed",   DATA_COND, wind_ok, DATA_FORMAT, "%.1f m/s", DATA_DOUBLE, wind_avg,
            "wind_dir_deg",     "Direction",    DATA_COND, wind_ok, DATA_INT,    wind_dir,
            "rain_mm",          "Rain",         DATA_COND, !temp_ok, DATA_FORMAT, "%.1f mm", DATA_DOUBLE, rain_mm,
            "unknown",          "Unknown",      DATA_COND, unk_ok, DATA_INT,    unk_raw,
            "uv",               "UV",           DATA_COND, uv_ok, DATA_FORMAT, "%.1f", DATA_DOUBLE,    uv,
            "flags",            "Flags",        DATA_INT,    flags,
            "mic",              "Integrity",    DATA_STRING, "CRC",
            NULL);
    /* clang-format on */

    decoder_output_data(decoder, data);
    return 1;
}

/**
Decoder for Bresser Weather Center 5-in-1.

The compact 5-in-1 multifunction outdoor sensor transmits the data on 868.3 MHz.
The device uses FSK-PCM encoding,
The device sends a transmission every 12 seconds.
A transmission starts with a preamble of 0xAA.

Decoding borrowed from https://github.com/andreafabrizi/BresserWeatherCenter

Preamble:

    aa aa aa aa aa 2d d4

Packet payload without preamble (203 bits):

     0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25
    -----------------------------------------------------------------------------
    ee 93 7f f7 bf fb ef 9e fe ae bf ff ff 11 6c 80 08 40 04 10 61 01 51 40 00 00
    ed 93 7f ff 0f ff ef b8 fe 7d bf ff ff 12 6c 80 00 f0 00 10 47 01 82 40 00 00
    eb 93 7f eb 9f ee ef fc fc d6 bf ff ff 14 6c 80 14 60 11 10 03 03 29 40 00 00
    ed 93 7f f7 cf f7 ef ed fc ce bf ff ff 12 6c 80 08 30 08 10 12 03 31 40 00 00
    f1 fd 7f ff af ff ef bd fd b7 c9 ff ff 0e 02 80 00 50 00 10 42 02 48 36 00 00 00 00 (from https://github.com/merbanan/rtl_433/issues/719#issuecomment-388896758)
    ee b7 7f ff 1f ff ef cb fe 7b d7 fc ff 11 48 80 00 e0 00 10 34 01 84 28 03 00 (from https://github.com/andreafabrizi/BresserWeatherCenter)
    e3 fd 7f 89 7e 8a ed 68 fe af 9b fd ff 1c 02 80 76 81 75 12 97 01 50 64 02 00 00 00 (Large Wind Values, Gust=37.4m/s Avg=27.5m/s from https://github.com/merbanan/rtl_433/issues/1315)
    ef a1 ff ff 1f ff ef dc ff de df ff 7f 10 5e 00 00 e0 00 10 23 00 21 20 00 80 00 00 (low batt +ve temp)
    ed a1 ff ff 1f ff ef 8f ff d6 df ff 77 12 5e 00 00 e0 00 10 70 00 29 20 00 88 00 00 (low batt -ve temp -7.0C)
    ec 91 ff ff 1f fb ef e7 fe ad ed ff f7 13 6e 00 00 e0 04 10 18 01 52 12 00 08 00 00 (good batt -ve temp)
    CC CC CC CC CC CC CC CC CC CC CC CC CC uu II    GG DG WW  W TT  T HH RR  R Bt
                                              G-MSB ^     ^ W-MSB  (strange but consistent order)

- C = Check, inverted data of 13 byte further
- uu = checksum (number/count of set bits within bytes 14-25)
- I = station ID (maybe)
- G = wind gust in 1/10 m/s, normal binary coded, GGxG = 0x76D1 => 0x0176 = 256 + 118 = 374 => 37.4 m/s.  MSB is out of sequence.
- D = wind direction 0..F = N..NNE..E..S..W..NNW
- W = wind speed in 1/10 m/s, BCD coded, WWxW = 0x7512 => 0x0275 = 275 => 27.5 m/s. MSB is out of sequence.
- T = temperature in 1/10 °C, BCD coded, TTxT = 1203 => 31.2 °C
- t = temperature sign, minus if unequal 0
- H = humidity in percent, BCD coded, HH = 23 => 23 %
- R = rain in mm, BCD coded, RRxR = 1203 => 31.2 mm
- B = Battery. 0=Ok, 8=Low.
*/

static int bresser_5in1_decode(r_device *decoder, bitbuffer_t *bitbuffer)
{
    uint8_t const preamble_pattern[] = {0xaa, 0xaa, 0xaa, 0x2d, 0xd4};

    data_t *data;
    uint8_t msg[26];
    uint16_t sensor_id;
    unsigned len = 0;

    // piggy-back for now
    int ret = bresser_7in1_decode(decoder, bitbuffer);
    if (ret > 0)
        return ret;

    ret = bresser_6in1_decode(decoder, bitbuffer);
    if (ret > 0)
        return ret;

    if (bitbuffer->num_rows != 1
            || bitbuffer->bits_per_row[0] < 248
            || bitbuffer->bits_per_row[0] > 440) {
        if (decoder->verbose > 1) {
            fprintf(stderr, "%s: bit_per_row %u out of range\n", __func__, bitbuffer->bits_per_row[0]);
        }
        return DECODE_ABORT_EARLY; // Unrecognized data
    }

    unsigned start_pos = bitbuffer_search(bitbuffer, 0, 0,
            preamble_pattern, sizeof (preamble_pattern) * 8);

    if (start_pos == bitbuffer->bits_per_row[0]) {
        return DECODE_ABORT_LENGTH;
    }
    start_pos += sizeof (preamble_pattern) * 8;
    len = bitbuffer->bits_per_row[0] - start_pos;
    if (((len + 7) / 8) < sizeof (msg)) {
        if (decoder->verbose > 1) {
            fprintf(stderr, "%s: %u too short\n", __func__, len);
        }
        return DECODE_ABORT_LENGTH; // message too short
    }
    // truncate any excessive bits
    len = MIN(len, sizeof (msg) * 8);

    bitbuffer_extract_bytes(bitbuffer, 0, start_pos, msg, len);

    // First 13 bytes need to match inverse of last 13 bytes
    for (unsigned col = 0; col < sizeof (msg) / 2; ++col) {
        if ((msg[col] ^ msg[col + 13]) != 0xff) {
            if (decoder->verbose > 1) {
                fprintf(stderr, "%s: Parity wrong at %u\n", __func__, col);
            }
            return DECODE_FAIL_MIC; // message isn't correct
        }
    }

    // check popcount
    // uu = checksum (number/count of set bits within bytes 14-25)

    sensor_id = msg[14];

    int temp_raw = (msg[20] & 0x0f) + ((msg[20] & 0xf0) >> 4) * 10 + (msg[21] &0x0f) * 100;
    if (msg[25] & 0x0f)
        temp_raw = -temp_raw;
    float temperature = temp_raw * 0.1f;

    int humidity = (msg[22] & 0x0f) + ((msg[22] & 0xf0) >> 4) * 10;

    float wind_direction_deg = ((msg[17] & 0xf0) >> 4) * 22.5f;

    int gust_raw = ((msg[17] & 0x0f) << 8) + msg[16]; //fix merbanan/rtl_433#1315
    float wind_gust = gust_raw * 0.1f;

    int wind_raw = (msg[18] & 0x0f) + ((msg[18] & 0xf0) >> 4) * 10 + (msg[19] & 0x0f) * 100; //fix merbanan/rtl_433#1315
    float wind_avg = wind_raw * 0.1f;

    int rain_raw = (msg[23] & 0x0f) + ((msg[23] & 0xf0) >> 4) * 10 + (msg[24] & 0x0f) * 100;
    float rain = rain_raw * 0.1f;

    int battery_ok = ((msg[25] & 0x80) == 0);

    /* clang-format off */
    data = data_make(
            "model",            "",             DATA_STRING, "Bresser-5in1",
            "id",               "",             DATA_INT,    sensor_id,
            "battery",          "Battery",      DATA_STRING, battery_ok ? "OK": "LOW",
            "temperature_C",    "Temperature",  DATA_FORMAT, "%.1f C", DATA_DOUBLE, temperature,
            "humidity",         "Humidity",     DATA_INT, humidity,
            _X("wind_max_m_s","wind_gust"),        "Wind Gust",    DATA_FORMAT, "%.1f m/s",DATA_DOUBLE, wind_gust,
            _X("wind_avg_m_s","wind_speed"),       "Wind Speed",   DATA_FORMAT, "%.1f m/s",DATA_DOUBLE, wind_avg,
            "wind_dir_deg",     "Direction",    DATA_FORMAT, "%.1f",DATA_DOUBLE, wind_direction_deg,
            "rain_mm",          "Rain",         DATA_FORMAT, "%.1f mm",DATA_DOUBLE, rain,
            "mic",              "Integrity",    DATA_STRING, "CHECKSUM",
            NULL);
    /* clang-format on */

    decoder_output_data(decoder, data);
    return 1;
}

static char *output_fields[] = {
        "model",
        "id",
        "battery",
        "temperature_C",
        "humidity",
        "wind_gust",  // TODO: delete this
        "wind_speed", // TODO: delete this
        "wind_max_m_s",
        "wind_avg_m_s",
        "wind_dir_deg",
        "rain_mm",
        "uv",
        "mic",
        NULL,
};

r_device bresser_5in1 = {
        .name        = "Bresser Weather Center 5-in-1",
        .modulation  = FSK_PULSE_PCM,
        .short_width = 124,
        .long_width  = 124,
        .reset_limit = 25000,
        .decode_fn   = &bresser_5in1_decode,
        .disabled    = 0,
        .fields      = output_fields,
};
