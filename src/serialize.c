#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK

// Inclusão do cabeçalho do LittleFS apenas para ESP
#ifdef ESP_PLATFORM
//#include "esp_littlefs.h"   // Use este cabeçalho (a biblioteca exporta esp_vfs_littlefs_*)
//    #define FILE_PATH "/littlefs/world.bin"
//#else
//    #include <stdio.h>
 //   #define FILE_PATH "world.bin"
//#endif

#include "tools.h"
#include "registries.h"
#include "serialize.h"

int64_t last_disk_sync_time = 0;

// Restores world data from disk, or writes world file if it doesn't exist
int initSerializer() {

    last_disk_sync_time = get_program_time();

#ifdef ESP_PLATFORM
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",   // Nome da partição no partitions.csv
        .format_if_mount_failed = true,
        .dont_mount = false,
        // .read_only = false,
        // .grow_on_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        printf("LittleFS register error: %s\n", esp_err_to_name(ret));
        perror("Failed to mount LittleFS. Aborting.");
        return 1;
    }
#endif

    // Tenta abrir o arquivo existente
    FILE *file = fopen(FILE_PATH, "rb");
    if (file) {
        // Lê as mudanças de bloco
        size_t read = fread(block_changes, 1, sizeof(block_changes), file);
        if (read != sizeof(block_changes)) {
            printf("Read %u bytes from \"world.bin\", expected %u (block changes). Aborting.\n",
                   read, sizeof(block_changes));
            fclose(file);
            return 1;
        }

        // Recupera o contador de mudanças
        for (int i = 0; i < MAX_BLOCK_CHANGES; i++) {
            if (block_changes[i].block == 0xFF) continue;
            if (block_changes[i].block == B_chest) i += 14;
            if (i >= block_changes_count) block_changes_count = i + 1;
        }

        // Posiciona para ler os dados do jogador
        if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
            perror("Failed to seek to player data in \"world.bin\". Aborting.");
            fclose(file);
            return 1;
        }

        read = fread(player_data, 1, sizeof(player_data), file);
        fclose(file);
        if (read != sizeof(player_data)) {
            printf("Read %u bytes from \"world.bin\", expected %u (player data). Aborting.\n",
                   read, sizeof(player_data));
            return 1;
        }
    } else {
        // Arquivo não existe – cria um novo
        printf("No \"world.bin\" file found, creating one...\n\n");

        file = fopen(FILE_PATH, "wb");
        if (!file) {
            perror("Failed to open \"world.bin\" for writing.\n"
                   "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\".");
            return 1;
        }

        size_t written = fwrite(block_changes, 1, sizeof(block_changes), file);
        if (written != sizeof(block_changes)) {
            perror("Failed to write initial block data to \"world.bin\".");
            fclose(file);
            return 1;
        }

        if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
            perror("Failed to seek past block changes in \"world.bin\".");
            fclose(file);
            return 1;
        }

        written = fwrite(player_data, 1, sizeof(player_data), file);
        fclose(file);
        if (written != sizeof(player_data)) {
            perror("Failed to write initial player data to \"world.bin\".");
            return 1;
        }
    }

    return 0;
}

// Writes a range of block change entries to disk
void writeBlockChangesToDisk(int from, int to) {
    FILE *file = fopen(FILE_PATH, "r+b");
    if (!file) {
        perror("Failed to open \"world.bin\". Block updates have been dropped.");
        return;
    }

    for (int i = from; i <= to; i++) {
        if (fseek(file, i * sizeof(BlockChange), SEEK_SET) != 0) {
            fclose(file);
            perror("Failed to seek in \"world.bin\". Block updates have been dropped.");
            return;
        }
        if (fwrite(&block_changes[i], 1, sizeof(BlockChange), file) != sizeof(BlockChange)) {
            fclose(file);
            perror("Failed to write to \"world.bin\". Block updates have been dropped.");
            return;
        }
    }
    fclose(file);
}

// Writes all player data to disk
void writePlayerDataToDisk() {
    FILE *file = fopen(FILE_PATH, "r+b");
    if (!file) {
        perror("Failed to open \"world.bin\". Player updates have been dropped.");
        return;
    }

    if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
        fclose(file);
        perror("Failed to seek in \"world.bin\". Player updates have been dropped.");
        return;
    }

    if (fwrite(&player_data, 1, sizeof(player_data), file) != sizeof(player_data)) {
        fclose(file);
        perror("Failed to write to \"world.bin\". Player updates have been dropped.");
        return;
    }

    fclose(file);
}

// Writes data queued for interval writes, but only if enough time has passed
void writeDataToDiskOnInterval() {
    if (get_program_time() - last_disk_sync_time < DISK_SYNC_INTERVAL) return;
    last_disk_sync_time = get_program_time();

    writePlayerDataToDisk();
#ifdef DISK_SYNC_BLOCKS_ON_INTERVAL
    writeBlockChangesToDisk(0, block_changes_count);
#endif
}

#ifdef ALLOW_CHESTS
// Writes a chest slot change to disk
void writeChestChangesToDisk(uint8_t *storage_ptr, uint8_t slot) {
    int index = (int)(storage_ptr - (uint8_t *)block_changes) / sizeof(BlockChange) + slot / 2;
    writeBlockChangesToDisk(index, index);
}
#endif

#endif // SYNC_WORLD_TO_DISK
