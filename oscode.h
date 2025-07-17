#include "oscode.h"
#include "oscode2.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Game system constants
#define MAX_GAMES 256
#define MAX_GAME_NAME 64
#define MAX_SAVE_SLOTS 10
#define GAME_SIGNATURE 0x47414D45  // "GAME" in hex
#define SAVE_SIGNATURE 0x53415645  // "SAVE" in hex

// Game states
typedef enum {
    GAME_STATE_STOPPED = 0,
    GAME_STATE_LOADING = 1,
    GAME_STATE_RUNNING = 2,
    GAME_STATE_PAUSED = 3,
    GAME_STATE_SAVING = 4,
    GAME_STATE_ERROR = 5
} game_state_t;

// Game types
typedef enum {
    GAME_TYPE_ARCADE = 0,
    GAME_TYPE_PUZZLE = 1,
    GAME_TYPE_PLATFORM = 2,
    GAME_TYPE_SHOOTER = 3,
    GAME_TYPE_RPG = 4,
    GAME_TYPE_HOMEBREW = 5
} game_type_t;

// Game header structure
typedef struct {
    uint32_t signature;
    uint32_t version;
    char name[MAX_GAME_NAME];
    char author[32];
    game_type_t type;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t required_memory;
    uint32_t entry_point;
    uint32_t save_data_size;
    uint32_t checksum;
} game_header_t;

// Save game structure
typedef struct {
    uint32_t signature;
    uint32_t game_checksum;
    uint32_t save_time;
    uint32_t play_time;
    uint32_t level;
    uint32_t score;
    uint32_t data_size;
    uint8_t save_data[4096];  // Game-specific save data
} save_game_t;

// Game instance
typedef struct {
    game_header_t header;
    uint32_t process_id;
    game_state_t state;
    void* code_memory;
    void* data_memory;
    void* stack_memory;
    uint32_t start_time;
    uint32_t play_time;
    uint32_t current_level;
    uint32_t current_score;
    char save_path[MAX_PATH];
    bool has_save_data;
} game_instance_t;

// Game registry entry
typedef struct {
    char name[MAX_GAME_NAME];
    char path[MAX_PATH];
    game_type_t type;
    uint32_t size;
    uint32_t last_played;
    bool is_installed;
} game_registry_entry_t;

// Game manager context
typedef struct {
    fs_context_t* fs;
    memory_manager_t* mm;
    
    game_instance_t* current_game;
    game_registry_entry_t registry[MAX_GAMES];
    uint32_t game_count;
    
    // Runtime statistics
    uint32_t total_games_played;
    uint32_t total_play_time;
    uint32_t high_score;
    
    // System resources
    uint32_t max_game_memory;
    uint32_t available_memory;
    
    // Input state (simplified)
    struct {
        bool up, down, left, right;
        bool button_a, button_b, button_start, button_select;
        int mouse_x, mouse_y;
        bool mouse_click;
    } input;
    
    // Display buffer (simplified)
    uint32_t* framebuffer;
    uint32_t screen_width;
    uint32_t screen_height;
    
} game_manager_t;

// Game function pointer type
typedef int (*game_main_func)(game_manager_t* gm, void* game_data);

// Function prototypes
int game_system_init(game_manager_t* gm, fs_context_t* fs, memory_manager_t* mm);
int game_system_shutdown(game_manager_t* gm);

// Game management
int game_install(game_manager_t* gm, const char* game_path);
int game_uninstall(game_manager_t* gm, const char* game_name);
int game_load(game_manager_t* gm, const char* game_name);
int game_run(game_manager_t* gm);
int game_pause(game_manager_t* gm);
int game_resume(game_manager_t* gm);
int game_stop(game_manager_t* gm);

// Save system
int game_save(game_manager_t* gm, int slot);
int game_load_save(game_manager_t* gm, int slot);
int game_list_saves(game_manager_t* gm, const char* game_name, save_game_t* saves, int max_saves);

// Game registry
int game_scan_directory(game_manager_t* gm, const char* directory);
int game_list_installed(game_manager_t* gm, game_registry_entry_t* games, int max_games);
game_registry_entry_t* game_find_by_name(game_manager_t* gm, const char* name);

// Utility functions
uint32_t calculate_checksum(void* data, uint32_t size);
int validate_game_header(game_header_t* header);
void update_play_time(game_manager_t* gm);
void game_render_frame(game_manager_t* gm);
void game_update_input(game_manager_t* gm);

// Built-in demo games
int demo_game_pong(game_manager_t* gm, void* game_data);
int demo_game_tetris(game_manager_t* gm, void* game_data);
int demo_game_snake(game_manager_t* gm, void* game_data);

// Implementation

int game_system_init(game_manager_t* gm, fs_context_t* fs, memory_manager_t* mm) {
    memset(gm, 0, sizeof(game_manager_t));
    
    gm->fs = fs;
    gm->mm = mm;
    gm->max_game_memory = 16 * 1024 * 1024; // 16MB max per game
    gm->screen_width = 800;
    gm->screen_height = 600;
    
    // Allocate framebuffer
    gm->framebuffer = (uint32_t*)memory_alloc(mm, 
        gm->screen_width * gm->screen_height * sizeof(uint32_t), 
        MEM_TYPE_GRAPHICS);
    
    if (!gm->framebuffer) {
        printf("Failed to allocate framebuffer\n");
        return -1;
    }
    
    // Create games directory if it doesn't exist
    fs_mkdir(fs, "/games");
    fs_mkdir(fs, "/saves");
    
    // Scan for installed games
    game_scan_directory(gm, "/games");
    
    // Install built-in demo games
    printf("Installing built-in demo games...\n");
    
    // Create demo game entries
    game_registry_entry_t* pong = &gm->registry[gm->game_count++];
    strcpy(pong->name, "Pong");
    strcpy(pong->path, "builtin://pong");
    pong->type = GAME_TYPE_ARCADE;
    pong->size = 0;
    pong->is_installed = true;
    
    game_registry_entry_t* tetris = &gm->registry[gm->game_count++];
    strcpy(tetris->name, "Tetris");
    strcpy(tetris->path, "builtin://tetris");
    tetris->type = GAME_TYPE_PUZZLE;
    tetris->size = 0;
    tetris->is_installed = true;
    
    game_registry_entry_t* snake = &gm->registry[gm->game_count++];
    strcpy(snake->name, "Snake");
    strcpy(snake->path, "builtin://snake");
    snake->type = GAME_TYPE_ARCADE;
    snake->size = 0;
    snake->is_installed = true;
    
    printf("Game system initialized with %d games\n", gm->game_count);
    return 0;
}

int game_load(game_manager_t* gm, const char* game_name) {
    if (gm->current_game) {
        printf("Another game is already running. Stop it first.\n");
        return -1;
    }
    
    // Find game in registry
    game_registry_entry_t* entry = game_find_by_name(gm, game_name);
    if (!entry) {
        printf("Game '%s' not found\n", game_name);
        return -1;
    }
    
    // Allocate game instance
    gm->current_game = (game_instance_t*)memory_alloc(gm->mm, 
        sizeof(game_instance_t), MEM_TYPE_GAME);
    
    if (!gm->current_game) {
        printf("Failed to allocate memory for game instance\n");
        return -1;
    }
    
    game_instance_t* game = gm->current_game;
    memset(game, 0, sizeof(game_instance_t));
    
    // Handle built-in games
    if (strncmp(entry->path, "builtin://", 10) == 0) {
        // Set up built-in game header
        game->header.signature = GAME_SIGNATURE;
        game->header.version = 1;
        strcpy(game->header.name, entry->name);
        strcpy(game->header.author, "Built-in");
        game->header.type = entry->type;
        game->header.code_size = 0;
        game->header.data_size = 1024; // Small data allocation
        game->header.required_memory = 64 * 1024; // 64KB
        game->header.entry_point = 0;
        game->header.save_data_size = 512;
        
        // Allocate memory for built-in game
        game->data_memory = memory_alloc(gm->mm, game->header.data_size, MEM_TYPE_GAME);
        if (!game->data_memory) {
            memory_free(gm->mm, game);
            gm->current_game = NULL;
            return -1;
        }
        
        printf("Loaded built-in game: %s\n", game->header.name);
        game->state = GAME_STATE_LOADING;
        return 0;
    }
    
    // Load game from file system
    file_handle_t* game_file = fs_open(gm->fs, entry->path, 0x01); // Read mode
    if (!game_file) {
        printf("Failed to open game file: %s\n", entry->path);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    // Read game header
    if (fs_read(gm->fs, game_file, &game->header, sizeof(game_header_t)) != sizeof(game_header_t)) {
        printf("Failed to read game header\n");
        fs_close(game_file);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    // Validate game header
    if (validate_game_header(&game->header) != 0) {
        printf("Invalid game header\n");
        fs_close(game_file);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    // Check memory requirements
    if (game->header.required_memory > gm->max_game_memory) {
        printf("Game requires too much memory: %d bytes\n", game->header.required_memory);
        fs_close(game_file);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    // Allocate memory for game
    game->code_memory = memory_alloc(gm->mm, game->header.code_size, MEM_TYPE_GAME);
    game->data_memory = memory_alloc(gm->mm, game->header.data_size, MEM_TYPE_GAME);
    
    if (!game->code_memory || !game->data_memory) {
        printf("Failed to allocate memory for game\n");
        if (game->code_memory) memory_free(gm->mm, game->code_memory);
        if (game->data_memory) memory_free(gm->mm, game->data_memory);
        fs_close(game_file);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    // Read game code and data
    if (fs_read(gm->fs, game_file, game->code_memory, game->header.code_size) != game->header.code_size) {
        printf("Failed to read game code\n");
        memory_free(gm->mm, game->code_memory);
        memory_free(gm->mm, game->data_memory);
        fs_close(game_file);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    if (fs_read(gm->fs, game_file, game->data_memory, game->header.data_size) != game->header.data_size) {
        printf("Failed to read game data\n");
        memory_free(gm->mm, game->code_memory);
        memory_free(gm->mm, game->data_memory);
        fs_close(game_file);
        memory_free(gm->mm, game);
        gm->current_game = NULL;
        return -1;
    }
    
    fs_close(game_file);
    
    // Set up save path
    snprintf(game->save_path, MAX_PATH, "/saves/%s", game->header.name);
    
    game->state = GAME_STATE_LOADING;
    game->start_time = time(NULL);
    
    printf("Loaded game: %s by %s\n", game->header.name, game->header.author);
    printf("Memory allocated: Code=%d, Data=%d\n", game->header.code_size, game->header.data_size);
    
    return 0;
}

int game_run(game_manager_t* gm) {
    if (!gm->current_game) {
        printf("No game loaded\n");
        return -1;
    }
    
    game_instance_t* game = gm->current_game;
    
    if (game->state != GAME_STATE_LOADING && game->state != GAME_STATE_PAUSED) {
        printf("Game is not in a runnable state\n");
        return -1;
    }
    
    game->state = GAME_STATE_RUNNING;
    printf("Running game: %s\n", game->header.name);
    
    // Game main loop
    int result = 0;
    
    // Handle built-in games
    if (strcmp(game->header.name, "Pong") == 0) {
        result = demo_game_pong(gm, game->data_memory);
    } else if (strcmp(game->header.name, "Tetris") == 0) {
        result = demo_game_tetris(gm, game->data_memory);
    } else if (strcmp(game->header.name, "Snake") == 0) {
        result = demo_game_snake(gm, game->data_memory);
    } else {
        // Execute loaded game code
        if (game->code_memory && game->header.entry_point) {
            game_main_func main_func = (game_main_func)((char*)game->code_memory + game->header.entry_point);
            result = main_func(gm, game->data_memory);
        } else {
            printf("No executable code found\n");
            result = -1;
        }
    }
    
    // Update play time
    update_play_time(gm);
    
    if (result == 0) {
        printf("Game completed successfully\n");
    } else {
        printf("Game ended with error code: %d\n", result);
        game->state = GAME_STATE_ERROR;
    }
    
    return result;
}

int game_stop(game_manager_t* gm) {
    if (!gm->current_game) {
        return 0;
    }
    
    game_instance_t* game = gm->current_game;
    
    printf("Stopping game: %s\n", game->header.name);
    
    // Update statistics
    update_play_time(gm);
    gm->total_games_played++;
    gm->total_play_time += game->play_time;
    
    // Free game memory
    if (game->code_memory) {
        memory_free(gm->mm, game->code_memory);
    }
    if (game->data_memory) {
        memory_free(gm->mm, game->data_memory);
    }
    if (game->stack_memory) {
        memory_free(gm->mm, game->stack_memory);
    }
    
    // Free game instance
    memory_free(gm->mm, game);
    gm->current_game = NULL;
    
    printf("Game stopped and memory freed\n");
    return 0;
}

int game_save(game_manager_t* gm, int slot) {
    if (!gm->current_game || slot < 0 || slot >= MAX_SAVE_SLOTS) {
        return -1;
    }
    
    game_instance_t* game = gm->current_game;
    
    // Create save file path
    char save_path[MAX_PATH];
    snprintf(save_path, MAX_PATH, "%s_slot_%d.sav", game->save_path, slot);
    
    // Create save data
    save_game_t save_data;
    save_data.signature = SAVE_SIGNATURE;
    save_data.game_checksum = game->header.checksum;
    save_data.save_time = time(NULL);
    save_data.play_time = game->play_time;
    save_data.level = game->current_level;
    save_data.score = game->current_score;
    save_data.data_size = game->header.save_data_size;
    
    // Copy game-specific save data (simplified)
    memcpy(save_data.save_data, game->data_memory, 
           game->header.save_data_size < 4096 ? game->header.save_data_size : 4096);
    
    // Write save file
    file_handle_t* save_file = fs_open(gm->fs, save_path, 0x02); // Write mode
    if (!save_file) {
        printf("Failed to create save file: %s\n", save_path);
        return -1;
    }
    
    if (fs_write(gm->fs, save_file, &save_data, sizeof(save_game_t)) != sizeof(save_game_t)) {
        printf("Failed to write save data\n");
        fs_close(save_file);
        return -1;
    }
    
    fs_close(save_file);
    
    game->has_save_data = true;
    printf("Game saved to slot %d\n", slot);
    return 0;
}

game_registry_entry_t* game_find_by_name(game_manager_t* gm, const char* name) {
    for (uint32_t i = 0; i < gm->game_count; i++) {
        if (strcmp(gm->registry[i].name, name) == 0) {
            return &gm->registry[i];
        }
    }
    return NULL;
}

int validate_game_header(game_header_t* header) {
    if (header->signature != GAME_SIGNATURE) {
        printf("Invalid game signature\n");
        return -1;
    }
    
    if (header->version == 0) {
        printf("Invalid game version\n");
        return -1;
    }
    
    if (header->code_size == 0 && header->data_size == 0) {
        printf("Game has no code or data\n");
        return -1;
    }
    
    return 0;
}

void update_play_time(game_manager_t* gm) {
    if (gm->current_game) {
        uint32_t current_time = time(NULL);
        gm->current_game->play_time = current_time - gm->current_game->start_time;
    }
}

// Demo game implementations
int demo_game_pong(game_manager_t* gm, void* game_data) {
    printf("=== PONG ===\n");
    printf("Classic Pong game simulation\n");
    printf("Player 1: 5 | Player 2: 3\n");
    printf("Game Over - Player 1 Wins!\n");
    
    // Simulate game running for a bit
    for (int i = 0; i < 1000000; i++) {
        // Simulate game logic
        if (i % 100000 == 0) {
            printf("Game frame %d\n", i / 100000);
        }
    }
    
    gm->current_game->current_score = 5;
    gm->current_game->current_level = 1;
    
    return 0;
}

int demo_game_tetris(game_manager_t* gm, void* game_data) {
    printf("=== TETRIS ===\n");
    printf("Block puzzle game simulation\n");
    printf("Lines cleared: 15\n");
    printf("Level: 3\n");
    printf("Score: 12450\n");
    
    // Simulate game running
    for (int i = 0; i < 1500000; i++) {
        if (i % 150000 == 0) {
            printf("Piece %d placed\n", i / 150000);
        }
    }
    
    gm->current_game->current_score = 12450;
    gm->current_game->current_level = 3;
    
    return 0;
}

int demo_game_snake(game_manager_t* gm, void* game_data) {
    printf("=== SNAKE ===\n");
    printf("Snake game simulation\n");
    printf("Length: 8\n");
    printf("Score: 80\n");
    printf("Game Over - Snake hit wall!\n");
    
    // Simulate game running
    for (int i = 0; i < 800000; i++) {
        if (i % 100000 == 0) {
            printf("Snake length: %d\n", 3 + i / 100000);
        }
    }
    
    gm->current_game->current_score = 80;
    gm->current_game->current_level = 1;
    
    return 0;
}

int game_list_installed(game_manager_t* gm, game_registry_entry_t* games, int max_games) {
    int count = 0;
    for (uint32_t i = 0; i < gm->game_count && count < max_games; i++) {
        if (gm->registry[i].is_installed) {
            games[count] = gm->registry[i];
            count++;
        }
    }
    return count;
}

int game_scan_directory(game_manager_t* gm, const char* directory) {
    // This would scan the filesystem for .game files
    // For now, we'll just return success
    printf("Scanning directory: %s\n", directory);
    return 0;
}

uint32_t calculate_checksum(void* data, uint32_t size) {
    uint32_t checksum = 0;
    uint8_t* bytes = (uint8_t*)data;
    
    for (uint32_t i = 0; i < size; i++) {
        checksum += bytes[i];
        checksum = (checksum << 1) | (checksum >> 31); // Rotate left
    }
    
    return checksum;
}

int game_system_shutdown(game_manager_t* gm) {
    // Stop current game if running
    if (gm->current_game) {
        game_stop(gm);
    }
    
    // Free framebuffer
    if (gm->framebuffer) {
        memory_free(gm->mm, gm->framebuffer);
    }
    
    printf("Game system shutdown complete\n");
    printf("Total games played: %d\n", gm->total_games_played);
    printf("Total play time: %d seconds\n", gm->total_play_time);
    
    return 0;
}

// Main function to demonstrate the system
int main() {
    printf("=== Gaming OS Console System ===\n");
    
    // Initialize file system
    fs_context_t fs;
    if (fs_init(&fs, 10000) != 0) {
        printf("Failed to initialize file system\n");
        return 1;
    }
    
    if (fs_format(&fs, "GameOS") != 0) {
        printf("Failed to format file system\n");
        return 1;
    }
    
    // Initialize memory manager
    memory_manager_t mm;
    if (memory_init(&mm, 128 * 1024 * 1024, 0x100000) != 0) {
        printf("Failed to initialize memory manager\n");
        return 1;
    }
    
    // Initialize game system
    game_manager_t gm;
    if (game_system_init(&gm, &fs, &mm) != 0) {
        printf("Failed to initialize game system\n");
        return 1;
    }
    
    // List available games
    printf("\n=== Available Games ===\n");
    game_registry_entry_t games[MAX_GAMES];
    int game_count = game_list_installed(&gm, games, MAX_GAMES);
    
    for (int i = 0; i < game_count; i++) {
        printf("%d. %s (Type: %d)\n", i + 1, games[i].name, games[i].type);
    }
    
    // Demo: Play each game
    printf("\n=== Game Demo Session ===\n");
    
    for (int i = 0; i < game_count; i++) {
        printf("\n--- Playing %s ---\n", games[i].name);
        
        if (game_load(&gm, games[i].name) == 0) {
            game_run(&gm);
            
            // Save game
            printf("Saving game...\n");
            game_save(&gm, 0);
            
            game_stop(&gm);
        }
        
        printf("Press Enter to continue...");
        getchar();
    }
    
    // Shutdown
    game_system_shutdown(&gm);
    
    // Show memory statistics
    uint32_t total, free, fragmentation;
    memory_get_stats(&mm, &total, &free, &fragmentation);
    printf("\nMemory Statistics:\n");
    printf("Total: %d bytes\n", total);
    printf("Free: %d bytes\n", free);
    printf("Fragmentation events: %d\n", fragmentation);
    
    return 0;
}
