#include "halloc.h"

#include "stack.h"
#include <stdio.h>
// #include <stdlib.h>

/*int main() {
	int cap = 100000;
	int **a = halloc(sizeof(int*) * cap);
	int len = 0;
	for (int i = 0; i < cap; i++) {
		int *v = halloc(sizeof(int));
		*v = i * 2;
		a[len++] = v;
		// dumpblocks();
	}

	while (len > 0) {
		int rg = rand() % len;
		int *tmp = a[rg];
		a[rg] = a[--len];
		free(tmp);
	}
	printf("%p\n", halloc_last);
	free(a);
}*/

/*int main() {
	int *i = halloc(sizeof(int));
	int *j = halloc(sizeof(int));
	int *k = halloc(sizeof(int));
	int *s = halloc(2000);
	halloc(2);
	free(s);
	free(k);
	halloc(sizeof(int));
}*/

typedef def_stack(int) intstack;

int main() {
	get_blocks = test_block;
	unget_blocks = untest_block;
	intstack a;
	stinit(a);
	
	for (int i = 0; i < 10000; i++) {
		stpush(a, i*2);
	}

	while (a.len > 0) {
		printf("%d\n", stpop(a));
	}
	
	stfree(a);
	printf("%p\n", halloc_last);
}
