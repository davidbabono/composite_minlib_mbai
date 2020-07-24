#include "string.h"
#include "kernel.h"

#define PRINTK_BUFFER 1024
#define MAX_HANDLERS 5

static void (*printk_handlers[MAX_HANDLERS])(const char *);
static unsigned num_handlers = 0;

int
printk_register_handler(void (*handler)(const char *))
{
	if (handler == NULL || num_handlers > (sizeof(printk_handlers) / sizeof(printk_handlers[0]))) return -1;
	printk_handlers[num_handlers++] = handler;
	return 0;
}

void
printk(const char *fmt, ...)
{
	char         buffer[PRINTK_BUFFER];
	va_list      args;
	unsigned int l, i;

	va_start(args, fmt);
	l = vsprintf(buffer, fmt, args);
	va_end(args);

	for (i = 0; i < num_handlers; i++) { printk_handlers[i](buffer); }
}

void
dbgprint(void)
{
	printk("Debug print string active\n");
}

void
undefined_dbgprint(void)
{
	printk("Undefined handler!!\n");
}

void
prefetch_abort_dbgprint(void)
{
	printk("Prefetch Abort handler!!\n");
}

void
data_abort_dbgprint(void)
{
	printk("Data Abort handler!!\n");
}

void
fiq_dbgprint(void)
{
	printk("FIQ handler!!\n");
}

void
dbgval(unsigned long val)
{
	printk("Debug value %x\n", val);
}
