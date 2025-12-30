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

void
kinit()
{
  initlock(&kmem.lock, "kmem");
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
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void*
superalloc(void)
{
  acquire(&kmem.lock);
  struct run *r, *start = 0;
  int count = 0;
  uint64 start_pa = 0;
  for(r = kmem.freelist; r; r = r->next){
    if(count == 0){
      start = r;
      start_pa = (uint64)r;
      count = 1;
    } else if((uint64)r == (uint64)(start + count * 4096)){
      count++;
      if(count == 512){
        break;
      }
    } else {
      count = 0;
    }
  }
  if(count < 512){
    release(&kmem.lock);
    return 0; // 没有足够的连续内存
  }
  struct run *prev = 0;
  for(r = kmem.freelist; r && r != start; prev = r, r = r->next);
  if(prev)
    prev->next = start->next;
  else
    kmem.freelist = start->next;
  release(&kmem.lock);
  if(start_pa % (2*1024*1024) != 0){
    panic("superalloc: not 2MB aligned");
  }
  memset((void*)start_pa, 0, 2*1024*1024);
  
  return (void*)start_pa;
}
void
superfree(void *pa)
{
  if((uint64)pa % (2*1024*1024) != 0)
    panic("superfree: not 2MB aligned");
  acquire(&kmem.lock);
  for(int i = 0; i < 512; i++){
    struct run *r = (struct run*)((char*)pa + i * 4096);
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  release(&kmem.lock);
}
