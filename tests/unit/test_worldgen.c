#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "worldgen.h"
#include "globals.h"
#include "registries.h"

int main(void) {
    uint8_t h1[4096], h2[4096];
    precomputeChunkData(0, 0);
    uint8_t b = buildChunkSection(0, 0, 0);
    memcpy(h1, chunk_section, 4096);
    (void)b;

    precomputeChunkData(0, 0);
    buildChunkSection(0, 0, 0);
    memcpy(h2, chunk_section, 4096);

    assert(memcmp(h1, h2, 4096) == 0);
    printf("worldgen determinism: OK\n");

    precomputeChunkData(16, -32);
    buildChunkSection(16, 32, -32);
    printf("worldgen cross-chunk: OK\n");

    precomputeChunkData(-16, -32);
    buildChunkSection(-16, 0, -32);
    printf("worldgen negative-chunk: OK\n");

    precomputeChunkData(0, 0);
    assert(getBlockAt(-1, 70, -1) == getBlockAt(7, 70, 7));
    assert(getBlockAt(0, 81, 0) == B_air);
    assert(getBlockAt(0, -1, 0) == B_bedrock);
    printf("worldgen block bounds: OK\n");

    precomputeChunkData(0, 0);
    assert(getHeightAt(3, 5) == getHeightAt(3, 5));
    assert(getHeightAt(3, 5) >= TERRAIN_BASE_HEIGHT && getHeightAt(3, 5) <= 80);
    uint8_t biome = getChunkBiome(0, 0);
    assert(biome < 4);
    printf("worldgen terrain: OK\n");

    printf("ALL PASS\n");
    return 0;
}
