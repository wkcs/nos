/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/mm.h>
#include <assert.h>
#include <string.h>
static struct mm_node node;
extern int mm_buddy_init(struct mm_buddy *buddy);
extern addr_t __alloc_pages(struct mm_buddy *buddy, gfp_t flag, u32 order);
extern int __free_pages(addr_t addr, u32 order);
struct mm_node *find_mm_node(addr_t addr) { return addr >= node.start && addr < node.start + node.page_num * CONFIG_PAGE_SIZE ? &node : NULL; }
int main(void)
{
    void *memory;
    struct page *pages;
    addr_t first, second;
    assert(posix_memalign(&memory, CONFIG_PAGE_SIZE, 8 * CONFIG_PAGE_SIZE) == 0);
    memset(memory, 0, 8 * CONFIG_PAGE_SIZE);
    node.name = "test";
    node.start = (addr_t)memory;
    node.start_pfn = GET_PFN(node.start);
    node.page_num = 8;
    node.buddy_page_index = 1;
    node.free_num = 7;
    pages = memory;
    for (u32 i = 0; i < node.page_num; i++) { pages[i].node = &node; pages[i].pfn = node.start_pfn + i; pages[i].private = -1; }
    assert(mm_buddy_init(&node.buddy) == 0);
    first = __alloc_pages(&node.buddy, 0, 0);
    assert(first != 0);
    assert(__free_pages(first, 0) == 0);
    assert(node.buddy.info[0].free_num == 1);
    second = __alloc_pages(&node.buddy, 0, 0);
    assert(second == first);
    assert(node.buddy.info[0].free_num == 0);
    assert(__free_pages(second, 0) == 0);
    free(memory);
    return 0;
}
