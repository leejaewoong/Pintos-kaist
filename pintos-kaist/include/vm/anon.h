#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
#include "lib/kernel/bitmap.h"

struct page;
enum vm_type;

// ///// 추가 /////
// struct bitmap swap_table;

struct anon_page {            
    size_t swap_idx;    
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
