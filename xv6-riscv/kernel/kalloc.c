// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

char swap_bitmap[SWAPMAX/PGSIZE];

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  for (int i = 0; i < PHYSTOP / PGSIZE; i++) {
    pages[i].next = 0;
    pages[i].prev = 0;
    pages[i].pagetable = 0;
    pages[i].vaddr = 0;
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(!r){
    printf("error: OOM\n");
    return 0;
  }

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
  }

  return (void*)r;
}

void
insert_lru(struct page *p)
{
  if (page_lru_head == 0){
    page_lru_head = p;
    p->next = p;
    p->prev = p;
  }
  else{
    struct page *tail = page_lru_head->prev;
    tail->next = p;
    p->prev = tail;
    p->next = page_lru_head;
    page_lru_head->prev = p;
  }
}

void
remove_lru(struct page *p)
{
  if (p->next == p && p->prev == p){
    page_lru_head = 0;
  }
  else {
    p->prev->next = p->next;
    p->next->prev = p->prev;
    if (page_lru_head == p) {
      page_lru_head = p->next;
    }
  }
  p->next = 0;
  p->prev = 0;
}

// select victim with clock algorithm
struct page*
select_victim()
{
  acquire(&kmem.lock);

  if (page_lru_head == 0){
    release(&kmem.lock);
    return 0;
  }

  struct page *cur = page_lru_head;

  printf("[SELECT] checking page %p (va=%p)\n", cur, cur->vaddr);

  while (1) {
        if (cur->pagetable == 0 || cur->vaddr == 0) {
            panic("select_victim: invalid pagetable or vaddr");
        }

        pte_t *pte = walk(cur->pagetable, (uint64)cur->vaddr, 0);

        if (pte == 0) {
            panic("select_victim: invalid PTE");
        }

        if (*pte & PTE_A) {
            *pte &= ~PTE_A;

            // move to tail
            remove_lru(cur);
            insert_lru(cur);

            page_lru_head = page_lru_head->next;
            cur = page_lru_head;
        } else {
            break;
        }
    }

    release(&kmem.lock);
    return cur;
}

int
find_free_swappage()
{
  for (int i = 0; i < SWAPMAX/PGSIZE; i++) {
    if (swap_bitmap[i] == 0) {
      return i;
    }
  }
  return -1;
}

// swap in, swap out
void
manage_swappages(int op, pde_t* pagetable, char* pa, char* va)
{
  int idx = ((uint64)pa - KERNBASE) / PGSIZE;
  struct page *p =&pages[idx];

  // swap in
  acquire(&kmem.lock);

  switch (op){
    // new physical memory and mapping
    case ALLOC:
      p->pagetable = pagetable;
      p->vaddr = va;
      insert_lru(p);
      break;

    // free physical page and unmap
    case FREE:
      p->pagetable = 0;
      p->vaddr = 0;
      remove_lru(p);
      break;

    // swap in
    case SWAPIN:
      p->pagetable = pagetable;
      p->vaddr = va;
      insert_lru(p);
      break;

    // swap out
    case SWAPOUT:
      p->pagetable = 0;
      p->vaddr = 0;
      remove_lru(p);
      break;

    default:
      panic("manage swappages: error");
  }

  release(&kmem.lock);
}

void
swapout()
{
  // select victim, find victim PTE
  struct page *victim = select_victim();

  if (victim == 0)
    panic("swapout: no victim page");

  pte_t *pte = walk(victim->pagetable, (uint64)victim->vaddr, 0);

  if (pte == 0 || (*pte & PTE_V) == 0)
    panic("swapout: invalid PTE");

  uint64 pa = PTE2PA(*pte);
  
  // find free swap space
  int blkno = find_free_swappage();

  if (blkno == -1)
    panic("swapout: no free swap slot");
  swap_bitmap[blkno] = 1;

  // 1. swapwrite function to write the victim page in swap space
  swapwrite(pa, blkno);

  // 2. PPN offset , 3. clear PTE_V (Valid bit)
  *pte = (blkno << 12) | (PTE_FLAGS(*pte) & ~PTE_V);

  manage_swappages(SWAPOUT, victim->pagetable, (char*)pa, victim->vaddr);

  kfree((void*)pa);
}

void
swapin(pagetable_t pagetable, uint64 va)
{
  // 1. Get a new physical page
  char *pa = kalloc();

  if(pa == 0)
      panic("swapin: kalloc failed");

  pte_t *pte = walk(pagetable, va, 0);

  if(pte == 0)
      panic("swapin: walk failed");

  int blkno = PTE2PA(*pte) >> 12;

  // 2. use the swapread function to load from the swap space into the physical page
  swapread((uint64)pa, blkno);
  swap_bitmap[blkno] = 0;

  // 3. update the PPN value to the physical ddress of the physical page
  *pte = PA2PTE(pa) | PTE_FLAGS(*pte) | PTE_V;

  manage_swappages(SWAPIN, pagetable, pa, (char*) va);
}