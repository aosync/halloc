#include "halloc.h"

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALLOC_OFF(a, b) ((alloc_header_t*)((uint8_t*)(a) + (b)))
#define ALLOC_OFF_PREV(a, b) ((alloc_header_t*)((uint8_t*)(a) - (b)))
#define GETOFF(a, b) ((uint32_t)((uint8_t*)(a) - (uint8_t*)b))
#define WITHIN_BLOCK(a, b, o) ((size_t)(a) + (a)->size <= (size_t)(b) + (b)->size * HALLOC_BLOCK - (o))

/*
 * the halloc haiku:
 * brave soul to look at
 * a cursed allocator,
 * in need for rewrite
 */


block_header_t *halloc_last = NULL;
void *(*get_blocks)(uint64_t) = NULL;
void (*unget_blocks)(void*,uint64_t) = NULL;

#ifdef HALLOC_POSIX_HOST
void *test_block(uint64_t count) {
	return mmap(NULL, count * HALLOC_BLOCK, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANON, -1, 0);
}

void untest_block(void *ptr, uint64_t count) {
	munmap(ptr, count * HALLOC_BLOCK);
}
#endif

static void init_block(block_header_t *block, size_t blocksize) {
	*block = (block_header_t) {
		.next = NULL,
		.size = blocksize,
		.freelist = sizeof(block_header_t)
	};

	alloc_header_t *first_free = ALLOC_OFF(block, block->freelist);
	*first_free = (alloc_header_t) {
		.size = blocksize * HALLOC_BLOCK - sizeof(block_header_t) - 1,
		.next = 0,
		.bef = 0,
		.prev = 0,
		.meta = 0,
	};
}

static block_header_t *create_block(size_t bytes) {
		size_t blocksize = ((bytes + sizeof(block_header_t)) / HALLOC_BLOCK) + 1;
		block_header_t *tmp = get_blocks(blocksize);
		init_block(tmp, blocksize);
		return tmp;
}

static void freelist_insert(block_header_t *block, alloc_header_t *what, uint32_t sp) {
	alloc_header_t *placement;

	if (block->freelist == 0) {
		what->bef = 0;
		block->freelist = GETOFF(what, block);
		what->next = 0;
		return;
	}

	if (sp != 0)
		placement = ALLOC_OFF(block, sp);
	else
		placement = ALLOC_OFF(block, block->freelist);
	
	alloc_header_t *prev = NULL;

	// place remaining before second largest
	while (placement) {
		// if remains fit here, insert then go out
		if (what->size >= placement->size) {
			if (prev == NULL) {
				block->freelist = GETOFF(what, block);
				what->bef = 0;
			} else {
				prev->next = GETOFF(what, block);
				what->bef = GETOFF(prev, block);
			}
			what->next = GETOFF(placement, block);
			placement->bef = GETOFF(what, block);
			break;
		}

		// if placement doesn't have next, then we place it here
		if (placement->next == 0) {
			placement->next = GETOFF(what, block);
			what->bef = GETOFF(placement, block);
			what->next = 0;
			break;
		}

		// otherwise let's continue
		prev = placement;
		placement = ALLOC_OFF(block, placement->next);
	}
}

static void freelist_take(block_header_t *block, alloc_header_t *what) {
	if (what->bef == 0) {
		block->freelist = what->next;
	} else {
		alloc_header_t *bef = ALLOC_OFF(block, what->bef);
		bef->next = what->next;
	}

	if (what->next != 0) {
		alloc_header_t *next = ALLOC_OFF(block, what->next);
		next->bef = what->bef;
	}
	what->bef = 0;
	what->next = 0;
}

static alloc_header_t *freelist_peek(block_header_t *block) {
	if (block->freelist == 0)
		return NULL;

	alloc_header_t *h = ALLOC_OFF(block, block->freelist);
	return h;
}

static alloc_header_t *split(block_header_t *block, alloc_header_t *cur, size_t size) {
	if (cur->meta & 1 || cur->size < size)
		return NULL;
		// FREE AND GOOD SIZE

	freelist_take(block, cur);

	if (cur->size - size <= sizeof(alloc_header_t)) {
		size = cur->size;
	}
	else {
		alloc_header_t *remains = ALLOC_OFF(cur, size);
		*remains = (alloc_header_t) {
			.prev = size,
			.size = cur->size - size,
			.meta = 0,
			.next = 0,
		};

		freelist_insert(block, remains, cur->next);
	}

	cur->size = size;
	cur->next = 0;
	cur->bef = 0;
	cur->meta |= 1;
	block->rc++;
	return cur;
}

alloc_header_t *march(block_header_t *block, size_t size) {
	alloc_header_t *cur = freelist_peek(block);
	if (cur == NULL)
		goto full;

	alloc_header_t *h = split(block, cur, size);
	if (h) {
		return h;
	}

full:
	if (block->next == NULL) {
		block_header_t *tmp = create_block(size);
		if (tmp != NULL) {
			tmp->next = halloc_last;
			halloc_last = tmp;
			return march(tmp, size);
		}
	}
	if (block->next) {
		return march(block->next, size);
	}
	return NULL;
}

void *malloc(size_t size) {
	return realloc(NULL, size);
}

void free(void *ptr) {
	block_header_t *block = halloc_last;
	block_header_t *pblock = NULL;
	while (block) {
		if ((size_t)block < (size_t)ptr && (size_t)ptr < (size_t)block + block->size * HALLOC_BLOCK) {
			break;
		}
		pblock = block;
		block = block->next;
	}

	if (block == NULL)
		return;

	alloc_header_t *subject = ALLOC_OFF_PREV(ptr, sizeof(alloc_header_t));

	if (block->freelist == 0) {
		block->freelist = GETOFF(subject, block);
		subject->meta = 0;
		return;
	}

	// NOTE: do NOT freelist_take the subject here. it obviously
	// is not in the freelist

	// attempt to back collate
	if (subject->prev) {
		alloc_header_t *prev = ALLOC_OFF_PREV(subject, subject->prev);

		if (!(prev->meta & 1)) {
			// PREV FREE -> COLLATE
			prev->size += subject->size;
			freelist_take(block, prev);
			subject = prev;
		}
	}

	// and now attempt to front collate
	if (WITHIN_BLOCK(subject, block, sizeof(alloc_header_t))) {
		alloc_header_t *next = ALLOC_OFF(subject, subject->size);
		next->prev = subject->size;
		if (!(next->meta & 1)) {
			freelist_take(block, next);
			subject->size += next->size;
			if (WITHIN_BLOCK(next, block, sizeof(alloc_header_t))) {
				alloc_header_t *nextnext = ALLOC_OFF(next, next->size);
				nextnext->prev = subject->size;
			}
		}
	}

	subject->meta = 0;

	freelist_insert(block, subject, 0);

	block->rc--;
	if (block->rc == 0) {
		if (pblock)
			pblock->next = block->next;
		else
			halloc_last = NULL;
		unget_blocks(block, block->size);
	}
}

void *realloc(void *ptr, size_t size) {
	if (size == 0)
		return NULL;
	if (size > (uint32_t)-(sizeof(alloc_header_t) + sizeof(block_header_t)))
		return NULL;

	size_t realsize = size + sizeof(alloc_header_t);
	if (halloc_last == NULL)
		halloc_last = create_block(realsize);


	alloc_header_t *header = NULL;
	if (ptr == NULL) {
		header = march(halloc_last, realsize);
	}
	else {
		header = ALLOC_OFF_PREV(ptr, sizeof(alloc_header_t));
		
		block_header_t *block = halloc_last;
		while (block) {
			if ((size_t)block < (size_t)ptr && (size_t)ptr < (size_t)block + block->size * HALLOC_BLOCK) {
				break;
			}
			block = block->next;
		}		

		if (block == NULL)
			return NULL;

		if (header->size < realsize) {
			if (WITHIN_BLOCK(header, block, sizeof(alloc_header_t))) {
				alloc_header_t *next = ALLOC_OFF(header, header->size);
				alloc_header_t *h = split(block, next, realsize - header->size);
				if (h) {
					header->size += h->size;
				}
				else {
					h = malloc(size);
					for (int i = 0; i < header->size - sizeof(alloc_header_t); i++)
						((uint8_t*)h)[i] = ((uint8_t*)(header+1))[i];
					free(header + 1);
					header = ALLOC_OFF_PREV(h, sizeof(alloc_header_t));
				}
			}
		}
		else if (header->size > realsize) {
			// shrink
			if (WITHIN_BLOCK(header, block, sizeof(alloc_header_t))) {
				// look for next to merge with
				alloc_header_t *next = ALLOC_OFF(header, header->size);
				if (!(next->meta & 1)) {
					// OK, next free. good.
					// now i need to make
					alloc_header_t newnexttmp = *next;
					alloc_header_t *newnext = ALLOC_OFF(header, realsize);
					*newnext = newnexttmp;
					newnext->size += header->size - realsize;
					if (WITHIN_BLOCK(newnext, block, sizeof(alloc_header_t))) {
						alloc_header_t *nextnext = ALLOC_OFF(newnext, newnext->size);
						nextnext->prev = newnext->size;
					}

					// set the new size
					header->size = realsize;
					goto success;
				}

			}
			// 2 cases remaining:
			// - when the new space created is too small for a header and no free space after: do nothing [OK]
			// - when the space creates enough space for a header create an alloc

			if (header->size - realsize > sizeof(alloc_header_t)) {
				alloc_header_t *new = ALLOC_OFF(header, realsize);
				*new = (alloc_header_t) {
					.size = header->size - realsize,
					.prev = realsize,
					.meta = 0,
				};

				freelist_insert(block, new, 0);
				header->size = realsize;
			}
		}
	}
	

success:
	if (header == NULL)
		return NULL;

	return header + 1;
}

#ifdef HALLOC_POSIX_HOST
void dumpblock(block_header_t *block) {
	printf("block:\n\tnext(%p)\n\trc(%u)\n\tsize(%u)\n\tfreelist(%u)\n", block->next, block->rc, block->size, block->freelist);
	alloc_header_t *header = ALLOC_OFF(block, block->freelist);
	while (header) {
		printf("%u:\n\tnext(%u)\n\tbef(%u)\n\tsize(%u)\n\tprev(%u)\n\tmeta(%u)\n", GETOFF(header, block), header->next, header->bef, header->size, header->prev, header->meta);
		if (header->next == 0)
			break;
		header = ALLOC_OFF(block, header->next);
	}
}

void dumpblocks() {
	block_header_t *b = halloc_last;
	while (b) {
		dumpblock(b);
		printf("NB---\n");
		b = b->next;
	}
	printf("END---\n");
}
#endif
