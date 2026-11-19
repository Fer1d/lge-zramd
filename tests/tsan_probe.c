#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static int value;
static void *worker(void *unused){(void)unused;value=1;return NULL;}
int main(void){pthread_t t;assert(!pthread_create(&t,NULL,worker,NULL));assert(!pthread_join(t,NULL));assert(value==1);puts("TSan runtime probe PASS");return 0;}
