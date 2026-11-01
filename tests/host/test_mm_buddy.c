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
    assert(posix_memalign(&memory, 8 * CONFIG_PAGE_SIZE, 8 * CONFIG_PAGE_SIZE) == 0);
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
    for (u32 i = 0; i < CONFIG_MAX_ORDER; i++) pthread_mutex_destroy(&node.buddy.info[i].lock);
    node.page_num = 4;
    node.buddy_page_index = 2;
    node.free_num = 2;
    for (u32 i = 0; i < node.page_num; i++) pages[i].private = -1;
    assert(mm_buddy_init(&node.buddy) == 0);
    first = __alloc_pages(&node.buddy, 0, 0);
    second = __alloc_pages(&node.buddy, 0, 0);
    assert(first != 0 && second == first + CONFIG_PAGE_SIZE);
    assert(__free_pages(first, 0) == 0);
    assert(__free_pages(second, 0) == 0);
    assert(__alloc_pages(&node.buddy, 0, 1) == first);
    assert(node.free_num == 0);
    for (u32 i = 0; i < CONFIG_MAX_ORDER; i++) pthread_mutex_destroy(&node.buddy.info[i].lock);
    node.page_num = 8;
    node.buddy_page_index = 4;
    node.free_num = 4;
    for (u32 i = 0; i < node.page_num; i++) pages[i].private = -1;
    assert(mm_buddy_init(&node.buddy) == 0);
    {
        addr_t allocated[4];
        for (u32 i = 0; i < 4; i++) {
            allocated[i] = __alloc_pages(&node.buddy, 0, 0);
            assert(allocated[i] != 0);
        }
        assert(__free_pages(allocated[1], 0) == 0);
        assert(__free_pages(allocated[3], 0) == 0);
        assert(__free_pages(allocated[0], 0) == 0);
        assert(__free_pages(allocated[2], 0) == 0);
        assert(node.free_num == 4);
        assert(__alloc_pages(&node.buddy, 0, 2) == allocated[0]);
        assert(__free_pages(allocated[0], 2) == 0);
        first = __alloc_pages(&node.buddy, 0, 1);
        second = __alloc_pages(&node.buddy, 0, 1);
        assert(first == allocated[0] && second == first + 2 * CONFIG_PAGE_SIZE);
        assert(__free_pages(second, 1) == 0);
        assert(__free_pages(first, 1) == 0);
        assert(__alloc_pages(&node.buddy, 0, 2) == first);
        assert(__free_pages(first, 2) == 0);
    }
    for (u32 i = 0; i < CONFIG_MAX_ORDER; i++) pthread_mutex_destroy(&node.buddy.info[i].lock);

    /* Test 4: Unaligned buddy_page_index=1 with 4 usable pages (page_num=5) forms Order-2 block
     * and only links the head page into info[2].list (O(1) head-page invariant).
     */
    node.page_num = 5;
    node.buddy_page_index = 1;
    node.free_num = 4;
    for (u32 i = 0; i < node.page_num; i++) {
        pages[i].private = -1;
        pages[i].use_cnt = (i < node.buddy_page_index) ? 1 : 0;
        INIT_LIST_HEAD(&pages[i].list);
    }
    assert(mm_buddy_init(&node.buddy) == 0);
    assert(node.buddy.info[2].free_num == 1);
    assert(!list_empty(&pages[1].list));
    assert(list_empty(&pages[2].list));
    assert(list_empty(&pages[3].list));
    assert(list_empty(&pages[4].list));
    first = __alloc_pages(&node.buddy, 0, 2);
    assert(first == node.start + 1 * CONFIG_PAGE_SIZE);
    assert(node.free_num == 0);
    assert(__free_pages(first, 2) == 0);
    assert(node.free_num == 4);

    /* Test 5: __alloc_pages_exact(3 pages) returns trailing 4th page immediately to Order-0 */
    first = __alloc_pages_exact(&node.buddy, 3 * CONFIG_PAGE_SIZE, 0);
    assert(first == node.start + 1 * CONFIG_PAGE_SIZE);
    assert(node.free_num == 1);
    second = __alloc_pages(&node.buddy, 0, 0);
    assert(second == node.start + 4 * CONFIG_PAGE_SIZE);
    assert(node.free_num == 0);
    assert(__free_pages(second, 0) == 0);
    assert(__free_pages_exact(first, 3 * CONFIG_PAGE_SIZE) == 0);
    assert(node.free_num == 4);
    assert(node.buddy.info[2].free_num == 1);
    for (u32 i = 0; i < CONFIG_MAX_ORDER; i++) pthread_mutex_destroy(&node.buddy.info[i].lock);

    free(memory);
    return 0;
}
