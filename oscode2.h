#include <stdint.h>
#include <string.h>
#include <stdbool.h>

// Memory management constants
#define PAGE_SIZE 4096
#define MAX_PROCESSES 64
#define HEAP_SIZE (32 * 1024 * 1024)  // 32MB heap
#define STACK_SIZE (1024 * 1024)      // 1MB stack per process
#define KERNEL_HEAP_SIZE (8 * 1024 * 1024)  // 8MB kernel heap

// Memory types
typedef enum {
    MEM_TYPE_FREE = 0,
    MEM_TYPE_KERNEL = 1,
    MEM_TYPE_USER = 2,
    MEM_TYPE_GAME = 3,
    MEM_TYPE_GRAPHICS = 4,
    MEM_TYPE_AUDIO = 5,
    MEM_TYPE_RESERVED = 6
} memory_type_t;

// Memory block structure
typedef struct memory_block {
    uint32_t address;
    uint32_t size;
    memory_type_t type;
    uint32_t process_id;
    bool is_free;
    struct memory_block* next;
    struct memory_block* prev;
} memory_block_t;

// Page table entry
typedef struct {
    uint32_t present : 1;
    uint32_t writable : 1;
    uint32_t user : 1;
    uint32_t write_through : 1;
    uint32_t cache_disable : 1;
    uint32_t accessed : 1;
    uint32_t dirty : 1;
    uint32_t page_size : 1;
    uint32_t global : 1;
    uint32_t available : 3;
    uint32_t frame : 20;
} page_table_entry_t;

// Process memory context
typedef struct {
    uint32_t process_id;
    uint32_t page_directory;
    uint32_t heap_start;
    uint32_t heap_end;
    uint32_t stack_start;
    uint32_t stack_end;
    uint32_t code_start;
    uint32_t code_end;
    uint32_t total_allocated;
    bool is_active;
} process_memory_t;

// Memory manager context
typedef struct {
    uint32_t total_memory;
    uint32_t available_memory;
    uint32_t kernel_memory_start;
    uint32_t kernel_memory_end;
    uint32_t user_memory_start;
    uint32_t user_memory_end;
    
    memory_block_t* free_blocks;
    memory_block_t* allocated_blocks;
    
    process_memory_t processes[MAX_PROCESSES];
    uint32_t current_process;
    
    // Page management
    uint32_t* page_frames;
    uint32_t total_pages;
    uint32_t free_pages;
    
    // Heap management
    void* kernel_heap;
    void* user_heap;
    
    // Statistics
    uint32_t allocations;
    uint32_t deallocations;
    uint32_t fragmentation_count;
} memory_manager_t;

// Memory allocation request
typedef struct {
    uint32_t size;
    memory_type_t type;
    uint32_t alignment;
    uint32_t process_id;
    bool contiguous;
} memory_request_t;

// Function prototypes
int memory_init(memory_manager_t* mm, uint32_t total_memory, uint32_t kernel_start);
void* memory_alloc(memory_manager_t* mm, uint32_t size, memory_type_t type);
void* memory_alloc_aligned(memory_manager_t* mm, uint32_t size, uint32_t alignment, memory_type_t type);
int memory_free(memory_manager_t* mm, void* ptr);
int memory_realloc(memory_manager_t* mm, void** ptr, uint32_t new_size);

// Process memory management
int process_memory_create(memory_manager_t* mm, uint32_t process_id, uint32_t code_size);
int process_memory_destroy(memory_manager_t* mm, uint32_t process_id);
void* process_alloc(memory_manager_t* mm, uint32_t process_id, uint32_t size);
int process_free(memory_manager_t* mm, uint32_t process_id, void* ptr);

// Page management
uint32_t alloc_page(memory_manager_t* mm);
void free_page(memory_manager_t* mm, uint32_t page);
int map_page(memory_manager_t* mm, uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);
int unmap_page(memory_manager_t* mm, uint32_t virtual_addr);

// Utility functions
void memory_defragment(memory_manager_t* mm);
void memory_get_stats(memory_manager_t* mm, uint32_t* total, uint32_t* free, uint32_t* fragmentation);
memory_block_t* find_free_block(memory_manager_t* mm, uint32_t size, uint32_t alignment);
void merge_free_blocks(memory_manager_t* mm);

// Implementation

int memory_init(memory_manager_t* mm, uint32_t total_memory, uint32_t kernel_start) {
    memset(mm, 0, sizeof(memory_manager_t));
    
    mm->total_memory = total_memory;
    mm->available_memory = total_memory;
    mm->kernel_memory_start = kernel_start;
    mm->kernel_memory_end = kernel_start + KERNEL_HEAP_SIZE;
    mm->user_memory_start = mm->kernel_memory_end;
    mm->user_memory_end = total_memory;
    
    // Initialize page management
    mm->total_pages = total_memory / PAGE_SIZE;
    mm->free_pages = mm->total_pages;
    mm->page_frames = (uint32_t*)((char*)kernel_start + sizeof(memory_manager_t));
    
    // Mark all pages as free initially
    for (uint32_t i = 0; i < mm->total_pages; i++) {
        mm->page_frames[i] = 0; // 0 = free
    }
    
    // Create initial free block for user memory
    memory_block_t* initial_block = (memory_block_t*)mm->user_memory_start;
    initial_block->address = mm->user_memory_start + sizeof(memory_block_t);
    initial_block->size = mm->user_memory_end - initial_block->address;
    initial_block->type = MEM_TYPE_FREE;
    initial_block->process_id = 0;
    initial_block->is_free = true;
    initial_block->next = nullptr;
    initial_block->prev = nullptr;
    
    mm->free_blocks = initial_block;
    mm->allocated_blocks = nullptr;
    
    return 0;
}

void* memory_alloc(memory_manager_t* mm, uint32_t size, memory_type_t type) {
    return memory_alloc_aligned(mm, size, 4, type); // Default 4-byte alignment
}

void* memory_alloc_aligned(memory_manager_t* mm, uint32_t size, uint32_t alignment, memory_type_t type) {
    if (size == 0) return nullptr;
    
    // Find suitable free block
    memory_block_t* block = find_free_block(mm, size, alignment);
    if (!block) {
        // Try defragmentation
        memory_defragment(mm);
        block = find_free_block(mm, size, alignment);
        if (!block) return nullptr;
    }
    
    // Calculate aligned address
    uint32_t aligned_addr = (block->address + alignment - 1) & ~(alignment - 1);
    uint32_t padding = aligned_addr - block->address;
    
    // Split block if necessary
    if (block->size > size + padding + sizeof(memory_block_t)) {
        memory_block_t* new_block = (memory_block_t*)(aligned_addr + size);
        new_block->address = aligned_addr + size;
        new_block->size = block->size - size - padding;
        new_block->type = MEM_TYPE_FREE;
        new_block->process_id = 0;
        new_block->is_free = true;
        new_block->next = block->next;
        new_block->prev = block;
        
        if (block->next) {
            block->next->prev = new_block;
        }
        block->next = new_block;
        
        block->size = size + padding;
    }
    
    // Mark block as allocated
    block->type = type;
    block->process_id = mm->current_process;
    block->is_free = false;
    block->address = aligned_addr;
    
    // Move from free list to allocated list
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        mm->free_blocks = block->next;
    }
    
    if (block->next) {
        block->next->prev = block->prev;
    }
    
    // Add to allocated list
    block->prev = nullptr;
    block->next = mm->allocated_blocks;
    if (mm->allocated_blocks) {
        mm->allocated_blocks->prev = block;
    }
    mm->allocated_blocks = block;
    
    mm->allocations++;
    mm->available_memory -= size;
    
    return (void*)aligned_addr;
}

int memory_free(memory_manager_t* mm, void* ptr) {
    if (!ptr) return -1;
    
    uint32_t addr = (uint32_t)ptr;
    
    // Find block in allocated list
    memory_block_t* block = mm->allocated_blocks;
    while (block) {
        if (block->address == addr) {
            break;
        }
        block = block->next;
    }
    
    if (!block) return -1; // Block not found
    
    // Remove from allocated list
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        mm->allocated_blocks = block->next;
    }
    
    if (block->next) {
        block->next->prev = block->prev;
    }
    
    // Mark as free
    block->type = MEM_TYPE_FREE;
    block->process_id = 0;
    block->is_free = true;
    
    // Add to free list
    block->prev = nullptr;
    block->next = mm->free_blocks;
    if (mm->free_blocks) {
        mm->free_blocks->prev = block;
    }
    mm->free_blocks = block;
    
    mm->deallocations++;
    mm->available_memory += block->size;
    
    // Try to merge with adjacent free blocks
    merge_free_blocks(mm);
    
    return 0;
}

memory_block_t* find_free_block(memory_manager_t* mm, uint32_t size, uint32_t alignment) {
    memory_block_t* block = mm->free_blocks;
    memory_block_t* best_fit = nullptr;
    uint32_t best_size = UINT32_MAX;
    
    while (block) {
        if (block->is_free && block->size >= size) {
            uint32_t aligned_addr = (block->address + alignment - 1) & ~(alignment - 1);
            uint32_t padding = aligned_addr - block->address;
            
            if (block->size >= size + padding) {
                if (block->size < best_size) {
                    best_fit = block;
                    best_size = block->size;
                }
            }
        }
        block = block->next;
    }
    
    return best_fit;
}

void merge_free_blocks(memory_manager_t* mm) {
    memory_block_t* block = mm->free_blocks;
    
    while (block) {
        memory_block_t* next_block = block->next;
        
        // Check if we can merge with the next block
        if (next_block && next_block->is_free && 
            (block->address + block->size) == next_block->address) {
            
            // Merge blocks
            block->size += next_block->size;
            block->next = next_block->next;
            
            if (next_block->next) {
                next_block->next->prev = block;
            }
            
            // Free the merged block structure
            // Note: In a real implementation, you'd need proper cleanup
        }
        
        block = block->next;
    }
}

void memory_defragment(memory_manager_t* mm) {
    // Simple defragmentation - move allocated blocks to consolidate free space
    // This is a simplified version; real defragmentation is more complex
    
    memory_block_t* block = mm->allocated_blocks;
    uint32_t new_address = mm->user_memory_start;
    
    while (block) {
        if (!block->is_free) {
            // Move block data to new location
            if (block->address != new_address) {
                memmove((void*)new_address, (void*)block->address, block->size);
                block->address = new_address;
            }
            new_address += block->size;
        }
        block = block->next;
    }
    
    // Update free space
    if (mm->free_blocks) {
        mm->free_blocks->address = new_address;
        mm->free_blocks->size = mm->user_memory_end - new_address;
    }
    
    mm->fragmentation_count++;
}

int process_memory_create(memory_manager_t* mm, uint32_t process_id, uint32_t code_size) {
    if (process_id >= MAX_PROCESSES) return -1;
    
    process_memory_t* proc = &mm->processes[process_id];
    
    // Allocate code space
    proc->code_start = (uint32_t)memory_alloc(mm, code_size, MEM_TYPE_USER);
    if (!proc->code_start) return -1;
    proc->code_end = proc->code_start + code_size;
    
    // Allocate stack
    proc->stack_start = (uint32_t)memory_alloc(mm, STACK_SIZE, MEM_TYPE_USER);
    if (!proc->stack_start) {
        memory_free(mm, (void*)proc->code_start);
        return -1;
    }
    proc->stack_end = proc->stack_start + STACK_SIZE;
    
    // Set up heap bounds
    proc->heap_start = proc->code_end;
    proc->heap_end = proc->heap_start;
    
    proc->process_id = process_id;
    proc->total_allocated = code_size + STACK_SIZE;
    proc->is_active = true;
    
    return 0;
}

int process_memory_destroy(memory_manager_t* mm, uint32_t process_id) {
    if (process_id >= MAX_PROCESSES) return -1;
    
    process_memory_t* proc = &mm->processes[process_id];
    if (!proc->is_active) return -1;
    
    // Free all memory allocated to this process
    memory_block_t* block = mm->allocated_blocks;
    while (block) {
        memory_block_t* next = block->next;
        if (block->process_id == process_id) {
            memory_free(mm, (void*)block->address);
        }
        block = next;
    }
    
    proc->is_active = false;
    memset(proc, 0, sizeof(process_memory_t));
    
    return 0;
}

void* process_alloc(memory_manager_t* mm, uint32_t process_id, uint32_t size) {
    if (process_id >= MAX_PROCESSES) return nullptr;
    
    process_memory_t* proc = &mm->processes[process_id];
    if (!proc->is_active) return nullptr;
    
    uint32_t old_process = mm->current_process;
    mm->current_process = process_id;
    
    void* ptr = memory_alloc(mm, size, MEM_TYPE_USER);
    
    if (ptr) {
        proc->total_allocated += size;
    }
    
    mm->current_process = old_process;
    return ptr;
}

void memory_get_stats(memory_manager_t* mm, uint32_t* total, uint32_t* free, uint32_t* fragmentation) {
    *total = mm->total_memory;
    *free = mm->available_memory;
    *fragmentation = mm->fragmentation_count;
}

uint32_t alloc_page(memory_manager_t* mm) {
    for (uint32_t i = 0; i < mm->total_pages; i++) {
        if (mm->page_frames[i] == 0) {
            mm->page_frames[i] = 1; // Mark as allocated
            mm->free_pages--;
            return i * PAGE_SIZE;
        }
    }
    return 0; // No free pages
}

void free_page(memory_manager_t* mm, uint32_t page) {
    uint32_t page_index = page / PAGE_SIZE;
    if (page_index < mm->total_pages && mm->page_frames[page_index] == 1) {
        mm->page_frames[page_index] = 0;
        mm->free_pages++;
    }
}
