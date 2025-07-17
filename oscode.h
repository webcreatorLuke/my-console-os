#include <stdint.h>
#include <string.h>
#include <stdbool.h>

// File system constants
#define BLOCK_SIZE 512
#define MAX_FILENAME 64
#define MAX_FILES 1024
#define MAX_PATH 256
#define FS_MAGIC 0x434F4E53  // "CONS" in hex

// File types
typedef enum {
    FILE_TYPE_REGULAR = 0,
    FILE_TYPE_DIRECTORY = 1,
    FILE_TYPE_GAME = 2,
    FILE_TYPE_SAVE = 3
} file_type_t;

// File attributes
typedef struct {
    uint32_t size;
    uint32_t created_time;
    uint32_t modified_time;
    uint32_t accessed_time;
    uint16_t permissions;
    uint8_t type;
    uint8_t flags;
} file_attr_t;

// Directory entry
typedef struct {
    char name[MAX_FILENAME];
    uint32_t inode;
    file_attr_t attributes;
    uint32_t first_block;
    uint32_t next_entry;  // For linked directory entries
} dir_entry_t;

// Inode structure
typedef struct {
    uint32_t inode_num;
    file_attr_t attributes;
    uint32_t blocks[12];      // Direct blocks
    uint32_t indirect_block;  // Single indirect
    uint32_t double_indirect; // Double indirect
    uint32_t block_count;
} inode_t;

// Superblock - file system metadata
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t root_inode;
    uint32_t bitmap_blocks;
    uint32_t inode_table_blocks;
    uint32_t first_data_block;
    char volume_name[32];
} superblock_t;

// File system context
typedef struct {
    superblock_t sb;
    uint8_t* block_bitmap;
    uint8_t* inode_bitmap;
    inode_t* inode_table;
    uint8_t* data_blocks;
    uint32_t current_directory;
} fs_context_t;

// File handle
typedef struct {
    uint32_t inode;
    uint32_t position;
    uint8_t mode;  // read/write flags
    bool is_open;
} file_handle_t;

// Function prototypes
int fs_init(fs_context_t* ctx, uint32_t total_blocks);
int fs_format(fs_context_t* ctx, const char* volume_name);
int fs_create_file(fs_context_t* ctx, const char* path, file_type_t type);
int fs_delete_file(fs_context_t* ctx, const char* path);
file_handle_t* fs_open(fs_context_t* ctx, const char* path, uint8_t mode);
int fs_close(file_handle_t* handle);
int fs_read(fs_context_t* ctx, file_handle_t* handle, void* buffer, uint32_t size);
int fs_write(fs_context_t* ctx, file_handle_t* handle, const void* buffer, uint32_t size);
int fs_seek(file_handle_t* handle, uint32_t position);
int fs_mkdir(fs_context_t* ctx, const char* path);
int fs_list_directory(fs_context_t* ctx, const char* path, dir_entry_t* entries, uint32_t max_entries);

// Block allocation functions
uint32_t allocate_block(fs_context_t* ctx);
void free_block(fs_context_t* ctx, uint32_t block);
uint32_t allocate_inode(fs_context_t* ctx);
void free_inode(fs_context_t* ctx, uint32_t inode);

// Implementation

int fs_init(fs_context_t* ctx, uint32_t total_blocks) {
    memset(ctx, 0, sizeof(fs_context_t));
    
    // Initialize superblock
    ctx->sb.magic = FS_MAGIC;
    ctx->sb.version = 1;
    ctx->sb.block_size = BLOCK_SIZE;
    ctx->sb.total_blocks = total_blocks;
    ctx->sb.free_blocks = total_blocks;
    ctx->sb.total_inodes = MAX_FILES;
    ctx->sb.free_inodes = MAX_FILES;
    
    // Calculate layout
    ctx->sb.bitmap_blocks = (total_blocks + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    ctx->sb.inode_table_blocks = (MAX_FILES * sizeof(inode_t) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    ctx->sb.first_data_block = 1 + ctx->sb.bitmap_blocks + 1 + ctx->sb.inode_table_blocks;
    
    // Allocate memory for file system structures
    ctx->block_bitmap = (uint8_t*)calloc(ctx->sb.bitmap_blocks, BLOCK_SIZE);
    ctx->inode_bitmap = (uint8_t*)calloc(1, BLOCK_SIZE);
    ctx->inode_table = (inode_t*)calloc(MAX_FILES, sizeof(inode_t));
    ctx->data_blocks = (uint8_t*)calloc(total_blocks, BLOCK_SIZE);
    
    if (!ctx->block_bitmap || !ctx->inode_bitmap || !ctx->inode_table || !ctx->data_blocks) {
        return -1; // Memory allocation failed
    }
    
    return 0;
}

int fs_format(fs_context_t* ctx, const char* volume_name) {
    // Clear bitmaps
    memset(ctx->block_bitmap, 0, ctx->sb.bitmap_blocks * BLOCK_SIZE);
    memset(ctx->inode_bitmap, 0, BLOCK_SIZE);
    memset(ctx->inode_table, 0, MAX_FILES * sizeof(inode_t));
    
    // Set volume name
    strncpy(ctx->sb.volume_name, volume_name, 31);
    ctx->sb.volume_name[31] = '\0';
    
    // Mark system blocks as used
    for (uint32_t i = 0; i < ctx->sb.first_data_block; i++) {
        ctx->block_bitmap[i / 8] |= (1 << (i % 8));
        ctx->sb.free_blocks--;
    }
    
    // Create root directory
    ctx->sb.root_inode = allocate_inode(ctx);
    inode_t* root = &ctx->inode_table[ctx->sb.root_inode];
    root->inode_num = ctx->sb.root_inode;
    root->attributes.type = FILE_TYPE_DIRECTORY;
    root->attributes.permissions = 0755;
    root->attributes.size = 0;
    root->blocks[0] = allocate_block(ctx);
    root->block_count = 1;
    
    ctx->current_directory = ctx->sb.root_inode;
    
    return 0;
}

uint32_t allocate_block(fs_context_t* ctx) {
    if (ctx->sb.free_blocks == 0) return 0;
    
    for (uint32_t i = ctx->sb.first_data_block; i < ctx->sb.total_blocks; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        
        if (!(ctx->block_bitmap[byte_idx] & (1 << bit_idx))) {
            ctx->block_bitmap[byte_idx] |= (1 << bit_idx);
            ctx->sb.free_blocks--;
            return i;
        }
    }
    
    return 0; // No free blocks
}

void free_block(fs_context_t* ctx, uint32_t block) {
    if (block == 0 || block >= ctx->sb.total_blocks) return;
    
    uint32_t byte_idx = block / 8;
    uint32_t bit_idx = block % 8;
    
    if (ctx->block_bitmap[byte_idx] & (1 << bit_idx)) {
        ctx->block_bitmap[byte_idx] &= ~(1 << bit_idx);
        ctx->sb.free_blocks++;
    }
}

uint32_t allocate_inode(fs_context_t* ctx) {
    if (ctx->sb.free_inodes == 0) return 0;
    
    for (uint32_t i = 0; i < ctx->sb.total_inodes; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        
        if (!(ctx->inode_bitmap[byte_idx] & (1 << bit_idx))) {
            ctx->inode_bitmap[byte_idx] |= (1 << bit_idx);
            ctx->sb.free_inodes--;
            return i;
        }
    }
    
    return 0; // No free inodes
}

void free_inode(fs_context_t* ctx, uint32_t inode) {
    if (inode >= ctx->sb.total_inodes) return;
    
    uint32_t byte_idx = inode / 8;
    uint32_t bit_idx = inode % 8;
    
    if (ctx->inode_bitmap[byte_idx] & (1 << bit_idx)) {
        ctx->inode_bitmap[byte_idx] &= ~(1 << bit_idx);
        ctx->sb.free_inodes++;
    }
}

int fs_create_file(fs_context_t* ctx, const char* path, file_type_t type) {
    // Find parent directory and filename
    char dir_path[MAX_PATH];
    char filename[MAX_FILENAME];
    
    // Simple path parsing (you'd want more robust parsing)
    const char* last_slash = strrchr(path, '/');
    if (last_slash) {
        strncpy(dir_path, path, last_slash - path);
        dir_path[last_slash - path] = '\0';
        strncpy(filename, last_slash + 1, MAX_FILENAME - 1);
    } else {
        strcpy(dir_path, ".");
        strncpy(filename, path, MAX_FILENAME - 1);
    }
    filename[MAX_FILENAME - 1] = '\0';
    
    // Allocate inode
    uint32_t inode_num = allocate_inode(ctx);
    if (inode_num == 0) return -1;
    
    // Initialize inode
    inode_t* inode = &ctx->inode_table[inode_num];
    inode->inode_num = inode_num;
    inode->attributes.type = type;
    inode->attributes.permissions = 0644;
    inode->attributes.size = 0;
    inode->block_count = 0;
    
    // Add to parent directory (simplified - would need proper directory handling)
    // For now, just mark as created
    
    return 0;
}

file_handle_t* fs_open(fs_context_t* ctx, const char* path, uint8_t mode) {
    // Find file inode (simplified lookup)
    // In a real implementation, you'd traverse the directory structure
    
    file_handle_t* handle = (file_handle_t*)malloc(sizeof(file_handle_t));
    if (!handle) return NULL;
    
    handle->inode = 1; // Placeholder
    handle->position = 0;
    handle->mode = mode;
    handle->is_open = true;
    
    return handle;
}

int fs_close(file_handle_t* handle) {
    if (!handle || !handle->is_open) return -1;
    
    handle->is_open = false;
    free(handle);
    return 0;
}

int fs_read(fs_context_t* ctx, file_handle_t* handle, void* buffer, uint32_t size) {
    if (!handle || !handle->is_open) return -1;
    
    inode_t* inode = &ctx->inode_table[handle->inode];
    
    // Calculate which block contains the current position
    uint32_t block_idx = handle->position / BLOCK_SIZE;
    uint32_t block_offset = handle->position % BLOCK_SIZE;
    
    uint32_t bytes_read = 0;
    uint8_t* buf = (uint8_t*)buffer;
    
    while (bytes_read < size && handle->position < inode->attributes.size) {
        if (block_idx >= inode->block_count) break;
        
        uint32_t block_num = inode->blocks[block_idx];
        uint32_t bytes_to_read = BLOCK_SIZE - block_offset;
        
        if (bytes_to_read > size - bytes_read) {
            bytes_to_read = size - bytes_read;
        }
        
        if (bytes_to_read > inode->attributes.size - handle->position) {
            bytes_to_read = inode->attributes.size - handle->position;
        }
        
        memcpy(buf + bytes_read, 
               ctx->data_blocks + (block_num * BLOCK_SIZE) + block_offset, 
               bytes_to_read);
        
        bytes_read += bytes_to_read;
        handle->position += bytes_to_read;
        block_idx++;
        block_offset = 0;
    }
    
    return bytes_read;
}

int fs_write(fs_context_t* ctx, file_handle_t* handle, const void* buffer, uint32_t size) {
    if (!handle || !handle->is_open) return -1;
    
    inode_t* inode = &ctx->inode_table[handle->inode];
    
    // Allocate blocks if needed
    uint32_t blocks_needed = (handle->position + size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    
    while (inode->block_count < blocks_needed) {
        uint32_t new_block = allocate_block(ctx);
        if (new_block == 0) return -1; // No space
        
        if (inode->block_count < 12) {
            inode->blocks[inode->block_count] = new_block;
        }
        // TODO: Handle indirect blocks for larger files
        
        inode->block_count++;
    }
    
    // Write data (similar to read but writing)
    uint32_t block_idx = handle->position / BLOCK_SIZE;
    uint32_t block_offset = handle->position % BLOCK_SIZE;
    
    uint32_t bytes_written = 0;
    const uint8_t* buf = (const uint8_t*)buffer;
    
    while (bytes_written < size) {
        uint32_t block_num = inode->blocks[block_idx];
        uint32_t bytes_to_write = BLOCK_SIZE - block_offset;
        
        if (bytes_to_write > size - bytes_written) {
            bytes_to_write = size - bytes_written;
        }
        
        memcpy(ctx->data_blocks + (block_num * BLOCK_SIZE) + block_offset,
               buf + bytes_written,
               bytes_to_write);
        
        bytes_written += bytes_to_write;
        handle->position += bytes_to_write;
        block_idx++;
        block_offset = 0;
    }
    
    // Update file size
    if (handle->position > inode->attributes.size) {
        inode->attributes.size = handle->position;
    }
    
    return bytes_written;
}
