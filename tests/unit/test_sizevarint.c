#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "varnum.h"
#include "globals.h"

static uint8_t g_readbuf[64];
static size_t g_readpos = 0;
static size_t g_readlen = 0;
ssize_t recv_count = 0;

uint8_t readByte(int client_fd) {
    (void)client_fd;
    if (g_readpos >= g_readlen) { recv_count = 0; return 0; }
    recv_count = 1;
    return g_readbuf[g_readpos++];
}

ssize_t writeByte(int client_fd, uint8_t byte) {
    (void)client_fd; (void)byte;
    return 1;
}

static void set_input(const uint8_t *buf, size_t len) {
    if (len > sizeof(g_readbuf)) len = sizeof(g_readbuf);
    memcpy(g_readbuf, buf, len);
    g_readlen = len;
    g_readpos = 0;
}

static void test_sizeVarInt(void) {
    assert(sizeVarInt(0) == 1);
    assert(sizeVarInt(127) == 1);
    assert(sizeVarInt(128) == 2);
    assert(sizeVarInt(16383) == 2);
    assert(sizeVarInt(16384) == 3);
    assert(sizeVarInt(2097151) == 3);
    assert(sizeVarInt(2097152) == 4);
    assert(sizeVarInt(268435455) == 4);
    assert(sizeVarInt(268435456) == 5);
    printf("  sizeVarInt: OK\n");
}

static void test_readVarInt(void) {
    uint8_t buf[] = {0xAC, 0x02};
    set_input(buf, 2);
    int32_t v = readVarInt(0);
    assert(v == 300);
    printf("  readVarInt(300): OK\n");

    uint8_t b0[] = {0x00};
    set_input(b0, 1);
    assert(readVarInt(0) == 0);
    printf("  readVarInt(0): OK\n");

    uint8_t trunc[] = {0x80, 0x80};
    set_input(trunc, 2);
    assert(readVarInt(0) == (int32_t)VARNUM_ERROR);
    printf("  readVarInt(truncated): OK\n");

    uint8_t big[] = {0xFF, 0x3F};
    set_input(big, 2);
    int32_t r = readVarInt(0);
    assert(r == (int32_t)VARNUM_ERROR);
    printf("  readVarInt(oversize rejected): OK\n");
}

int main(void) {
    printf("test_sizevarint:\n");
    test_sizeVarInt();
    test_readVarInt();
    printf("ALL PASS\n");
    return 0;
}
