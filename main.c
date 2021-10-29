#include "halloc.h"

#include <stdio.h>

int main() {
	void *ptr = halloc(4);
	void *ptr2 = halloc(4);

	printf("%p and %p\n", ptr, ptr2);
}
