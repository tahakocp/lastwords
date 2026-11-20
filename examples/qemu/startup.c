#include <stdint.h>
#include <string.h>

extern uint32_t __data_start, __data_end, __data_load, __bss_start, __bss_end, __StackTop;
int main(void);

void Reset_Handler(void)
{
    memcpy(&__data_start, &__data_load, (uintptr_t)&__data_end - (uintptr_t)&__data_start);
    memset(&__bss_start, 0, (uintptr_t)&__bss_end - (uintptr_t)&__bss_start);
    main();
    for (;;) {
    }
}

static void Default_Handler(void)
{
    for (;;) {
    }
}

/* Weak, so lastwords' strong fault handlers take over. */
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".vectors"), used)) static void (*const vectors[16])(void) = {
    (void (*)(void))&__StackTop,
    Reset_Handler,
    Default_Handler, /* NMI */
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    Default_Handler, /* SVCall */
    Default_Handler, /* DebugMon */
    0,
    Default_Handler, /* PendSV */
    Default_Handler, /* SysTick */
};
