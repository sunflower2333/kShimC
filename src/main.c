#include <kshim.h>
#include <geni_uart.h>
#include <pl011_uart.h>
#include <timer.h>
#include <gicv3.h>

extern void _arm64_mmu_setup(void);

int kshim_main(
    uint64_t device_tree_address,
    uint64_t kernel_arg1,
    uint64_t kernel_arg2,
    uint64_t kernel_arg3)
{
    // Init GICv3
    // qemu
    gicv3_init(0x8000000, 0x80a0000, 0); // GICD_BASE, GICR_BASE, GICR_STRIDE

    // Timer init
    // qemu
    arm64_arch_timer_init(29, 30, 27, 26);

    // PL011 init
    // qemu
    pl011_init(0x9000000); // UART_BASE

    // Init other cores
    printf(CONFIG_BANNER"\n");
    // _arm64_mmu_setup();

    // Init UART
    // setup_se_geni_uart(0);
    // uint8_t msg[] = "This is a Test msg\n";

    // while (1)
    // {
    //     for(int i=0; i < 12; i++){
    //         while (((*(volatile uint32_t *)0x99C610) & (1<<30)) !=  (1<<30));
    //         *(volatile uint32_t *)0x99C270 = 4;
    //         *(volatile uint32_t *)0x99C600 = 0x8000000;
    //         *(volatile uint32_t *)0x99C700 = msg[i];
    //         while (((*(volatile uint32_t *)0x99C610) & (1<<30)) !=  (1<<30));
    //         }
    // }
    // // send message via UART
    // uint8_t message[] = "Hello, UART!\n";
    // while(1)
    //     geni_uart_write(0, message, 4);


    while (1) {
        // Wait for interrupt
        // volatile uint32_t *v = (uint32_t *)0x000000;
        // *v=1;

        // Read current ticks
        // uint64_t current_ticks = arm64_read_cntpct();
        // // Read compare value
        // uint64_t compare_value = arm64_read_cntpcval();
        // // Read remain ticks
        // uint64_t remain_ticks = arm64_read_cntptval();
        // printf("Current ticks: %lu, Compare value: %lu, Remain ticks: %lu\n", current_ticks, compare_value, remain_ticks);
        // Wait for interrupt
        asm volatile("wfi" : : : "memory");
    }
}