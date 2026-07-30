// // geni uart
#include <kshim.h>
#include <memops.h>
#include "qupv3.h"
#include "error_no.h"

#define REG_OUT REG32_OUT
#define REG_IN REG32_IN
#define REG_IN_MEM_BARRIER REG32_IN_MEM_BARRIER
#define REG_MSK_SET REG32_MSK_SET
#define REG_MSK_CLR REG32_MSK_CLR
#define REG_MSK_SFT_OUT REG32_MSK_SFT_OUT

// /*
//  * Reference:
//  *   linux/drivers/tty/serial/qcom_geni_serial.c
//  */
// #define MAX_POLL_TIME 200000

// uint8_t geni_poll_bit(uint64_t address, uint32_t bit, uint8_t expect)
// {
//     int i = 0;
//     while (i < MAX_POLL_TIME)
//     {
//         i++;
//         if ((REG_MSK_OUT(address, bit) == expect ? bit : 0))
//         {
//             return 1;
//         }
//         else
//         {
//             // TODO
//             // Implement delay function
//         }
//     }
//     return 0;
// }

// static inline void geni_poll_tx_finish()
// {
//     uint8_t tx_finished = geni_poll_bit(HWIO_SE_GENI_M_IRQ_STATUS, BIT_SE_GENI_M_IRQ_STATUS_M_CMD_DONE, 1);
//     if (!tx_finished) // Unlikely
//     {
//         // writel(M_GENI_CMD_ABORT, uport->membase +
//         // SE_GENI_M_CMD_CTRL_REG);
//         // qcom_geni_serial_poll_bit(uport, SE_GENI_M_IRQ_STATUS,
//         //                     M_CMD_ABORT_EN, true);
//         // writel(M_CMD_ABORT_EN, uport->membase + SE_GENI_M_IRQ_CLEAR);
//         REG_IN(HWIO_SE_GENI_M_CMD_CTRL_REG, BIT_SE_GENI_M_CMD_CTRL_REG_M_GENI_CMD_ABORT);
//         tx_finished = geni_poll_bit(HWIO_SE_GENI_M_IRQ_STATUS, BIT_SE_GENI_M_IRQ_STATUS_M_CMD_ABORT, 1);
//         REG_IN(HWIO_SE_GENI_M_IRQ_CLR, BIT_SE_GENI_M_CMD_CTRL_REG_M_GENI_CMD_ABORT);
//     }
// }

int setup_se_geni_uart(uint64_t mmio_base)
{
    // Verify proto (check if it's uart in devcfg)
    if (REG_MSK_SFT_OUT(HWIO_SE_GENI_FW_REVISION_RO, BIT_SE_GENI_FW_REVISION_RO_FW_REV_PROTOCOL, SFT_SE_GENI_FW_REVISION_RO_FW_REV_PROTOCOL) != QUPV3_UART)
    {
        return -ERROR_FW; // Not a UART device
    }

    // (((*(volatile uint32_t *)(((0x99c000) + 0x68))) & (((0x1U << (8)) - (0x1U << (15))))) >> (8));

    // Wait TX finished
    // geni_poll_tx_finish();

    // Abort RX
    {
        REG_IN(HWIO_SE_GENI_S_CMD_CTRL_REG, BIT_SE_GENI_S_CMD_ABORT);
        // uint8_t rx_finished = geni_poll_bit(HWIO_SE_GENI_S_CMD_CTRL_REG, BIT_SE_GENI_S_CMD_ABORT, 0);
        REG_IN(HWIO_SE_GENI_S_IRQ_CLEAR, BIT_SE_GENI_S_IRQ_CLEAR_CMD_ABORT | BIT_SE_GENI_S_IRQ_CLEAR_CMD_DONE);
        REG_IN(HWIO_SE_GENI_FORCE_DEFAULT_REG, BIT_SE_GENI_FORCE_DEFAULT_FORCE_DEFAULT);
    }

    /* 1. Pack and send se config */
    {
        uint32_t idx = 0;
        uint8_t bits_per_word = 8;
        uint8_t msb_to_lsb = 0; // LSB first

        // Vectors
        REG_IN(HWIO_SE_GENI_TX_PACKING_CFG0, ((((idx++) * 7) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC0_START_IDX) | (msb_to_lsb << SFT_SE_GENI_RTX_PACKING_CFGn_VEC0_DIRECTION) | ((bits_per_word - 1) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC0_LEN)) |
                                                 ((((idx++) * 7) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC1_START_IDX) | (msb_to_lsb << SFT_SE_GENI_RTX_PACKING_CFGn_VEC1_DIRECTION) | ((bits_per_word - 1) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC1_LEN)));
        REG_IN(HWIO_SE_GENI_TX_PACKING_CFG1, ((((idx++) * 7) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC0_START_IDX) | (msb_to_lsb << SFT_SE_GENI_RTX_PACKING_CFGn_VEC0_DIRECTION) | ((bits_per_word - 1) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC0_LEN)) |
                                                 ((((idx++) * 7) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC1_START_IDX) | (msb_to_lsb << SFT_SE_GENI_RTX_PACKING_CFGn_VEC1_DIRECTION) | ((bits_per_word - 1) << SFT_SE_GENI_RTX_PACKING_CFGn_VEC1_LEN)));
        REG_IN(HWIO_SE_GENI_RX_PACKING_CFG0, REG_OUT(HWIO_SE_GENI_TX_PACKING_CFG0));
        REG_IN(HWIO_SE_GENI_RX_PACKING_CFG1, REG_OUT(HWIO_SE_GENI_TX_PACKING_CFG1));
        REG_IN(HWIO_SE_GENI_BYTE_GRAN, bits_per_word / 16);
    }

    /* 2. Init se engine */
    {
        // Clear all IRQ
        {
            REG_IN(HWIO_SE_GSI_EVENT_EN, 0);
            REG_IN(HWIO_SE_GENI_M_IRQ_CLR, 0xffffffff);
            REG_IN(BIT_SE_GENI_S_IRQ_CLEAR_RX_FIFO_LAST, 0xffffffff);
            REG_IN(HWIO_SE_DMA_TX_IRQ_CLR, 0xffffffff);
            REG_IN(HWIO_SE_DMA_RX_IRQ_CLR, 0xffffffff);
            REG_IN(HWIO_SE_IRQ_EN, 0xffffffff);
        }

        // Initialize serial engine IO
        {
            REG_MSK_SET(HWIO_SE_GENI_CGC_CTRL, BIT_SE_GENI_CGC_CTRL_DEFAULT_CGC_EN);
            REG_MSK_SET(HWIO_SE_DMA_GENERAL_CFG, BIT_SE_DMA_GENERAL_CFG_AHB_SEC_SLV_CLK_CGC_ON | BIT_SE_DMA_GENERAL_CFG_DMA_AHB_SLV_CFG_ON |
                                                     BIT_SE_DMA_GENERAL_CFG_DMA_TX_CLK_CGC_ON | BIT_SE_DMA_GENERAL_CFG_DMA_RX_CLK_CGC_ON);
            REG_IN(HWIO_SE_GENI_OUTPUT_CTRL, BIT_SE_GENI_OUTPUT_CTRL_DEFAULT_IO_OUTPUT_CTRL);
            REG_IN(HWIO_SE_GENI_FORCE_DEFAULT_REG, BIT_SE_GENI_FORCE_DEFAULT_FORCE_DEFAULT);
        }

        // Setup serial engine IO mode
        {
            REG_MSK_SET(HWIO_SE_IRQ_EN, BIT_SE_IRQ_EN_DMA_RX_IRQ_EN | BIT_SE_IRQ_EN_DMA_TX_IRQ_EN | BIT_SE_IRQ_EN_GENI_M_IRQ_EN | BIT_SE_IRQ_EN_GENI_S_IRQ_EN);
            REG_MSK_CLR(HWIO_SE_GENI_DMA_MODE_EN, BIT_SE_GENI_DMA_MODE_EN_GENI_DMA_MODE_EN);
            REG_IN(HWIO_SE_GSI_EVENT_EN, 0);
        }

        // Set watermark
        REG_IN(HWIO_SE_GENI_RX_WATERMARK_REG, 16 / 2);     // DEF_FIFO_DEPTH_WORDS: 16
        REG_IN(HWIO_SE_GENI_RX_RFR_WATERMARK_REG, 16 - 2); // DEF_FIFO_DEPTH_WORDS: 16

        // Enable IRQ
        REG_IN(HWIO_SE_GENI_M_IRQ_EN, BIT_SE_GENI_M_IRQ_EN_M_CMD_OVERRUN_EN | BIT_SE_GENI_M_IRQ_EN_M_ILLEGAL_CMD_EN |
                                          BIT_SE_GENI_M_IRQ_EN_M_CMD_FAILURE_EN | BIT_SE_GENI_M_IRQ_EN_M_CMD_CANCEL_EN |
                                          BIT_SE_GENI_M_IRQ_EN_M_CMD_ABORT_EN | BIT_SE_GENI_M_IRQ_EN_M_TIMESTAMP_EN |
                                          BIT_SE_GENI_M_IRQ_EN_M_IO_DATA_DEASSERT_EN | BIT_SE_GENI_M_IRQ_EN_M_IO_DATA_ASSERT_EN |
                                          BIT_SE_GENI_M_IRQ_EN_M_RX_FIFO_RD_ERR_EN | BIT_SE_GENI_M_IRQ_EN_M_RX_FIFO_WR_ERR_EN |
                                          BIT_SE_GENI_M_IRQ_EN_M_TX_FIFO_RD_ERR_EN | BIT_SE_GENI_M_IRQ_EN_M_TX_FIFO_WR_ERR_EN);
        REG_IN(HWIO_SE_GENI_S_IRQ_EN, BIT_SE_GENI_S_IRQ_EN_S_CMD_OVERRUN_EN | BIT_SE_GENI_S_IRQ_EN_S_ILLEGAL_CMD_EN |
                                          BIT_SE_GENI_S_IRQ_EN_S_CMD_FAILURE_EN | BIT_SE_GENI_S_IRQ_EN_S_CMD_CANCEL_EN |
                                          BIT_SE_GENI_S_IRQ_EN_S_CMD_ABORT_EN | BIT_SE_GENI_S_IRQ_EN_S_GP_IRQ_0_EN |
                                          BIT_SE_GENI_S_IRQ_EN_S_GP_IRQ_1_EN | BIT_SE_GENI_S_IRQ_EN_S_GP_IRQ_2_EN |
                                          BIT_SE_GENI_S_IRQ_EN_S_GP_IRQ_3_EN | BIT_SE_GENI_S_IRQ_EN_S_GP_IRQ_4_EN |
                                          BIT_SE_GENI_S_IRQ_EN_S_RX_FIFO_RD_ERR_EN | BIT_SE_GENI_S_IRQ_EN_S_RX_FIFO_WR_ERR_EN);
    }

    /* Set serial engine to FIFO Mode */
    {
        // UART specific configuration
        // Clear DMA bit, actually it has been set before.
        REG_MSK_CLR(HWIO_SE_GENI_DMA_MODE_EN, BIT_SE_GENI_DMA_MODE_EN_GENI_DMA_MODE_EN);
    }

    /* Setup UART parameters */
    REG_IN(HWIO_SE_UART_TX_TRANS_CFG, 2);
    REG_IN(HWIO_SE_UART_TX_PARITY_CFG, 0);
    REG_IN(HWIO_SE_UART_RX_TRANS_CFG, 0);
    REG_IN(HWIO_SE_UART_RX_PARITY_CFG, 0);
    REG_IN(HWIO_SE_UART_TX_WORD_LEN, 8);
    REG_IN(HWIO_SE_UART_RX_WORD_LEN, 8);
    REG_IN(HWIO_SE_UART_TX_STOP_BIT_LEN, 0);

    /* Enable uart read */
    {
        uint32_t seq_cmd = 0;
        seq_cmd = REG_OUT(HWIO_SE_GENI_S_CMD0);
        seq_cmd &= ~(BIT_SE_GENI_S_CMD0_S_OPCODE | BIT_SE_GENI_S_CMD0_S_PARAMS);
        seq_cmd |= (OPCODE_SE_S_SE_UART_START_RX << SFT_SE_GENI_S_CMD0_S_OPCODE) | (0 & BIT_SE_GENI_S_CMD0_S_PARAMS); // cmd: UART_START_READ params: 0
    }

    return -ERROR_NONE;
}

void geni_uart_write(
    uint64_t mmio_base,
    const char *buf,
    uint32_t len)
{
    // REG_IN_MEM_BARRIER(HWIO_SE_GENI_TX_WATERMARK_REG, 2); // DEF_TX_WM: 2
    // REG_IN_MEM_BARRIER(HWIO_SE_GENI_M_IRQ_CLR, BIT_SE_GENI_M_IRQ_CLR_M_CMD_DONE_CLR);

    // Setup the TX buffer
    {
        REG_IN_MEM_BARRIER(HWIO_SE_UART_TX_TRANS_LEN, 4);
        REG_IN_MEM_BARRIER(HWIO_SE_GENI_M_CMD0, OPCODE_SE_M_UART_START_TX << SFT_SE_GENI_M_CMD0_M_OPCODE);
    }

    // Send buffer to fifo
    {
        uint32_t cache = 0;
        for (int i = 0; i < len; i++)
        {
            // Poll TX IRQ finish
            // if (geni_poll_bit(HWIO_SE_GENI_M_IRQ_STATUS, BIT_SE_GENI_M_IRQ_STATUS_M_TX_FIFO_WATERMARK, 1))
            // {
            //     break;
            // }

            // Write data to TX FIFO
            // Cache
            // cache = (cache >> 8) | (buf[i] << 24);
            if ((i & 3) == 3)
            {
                REG_IN_MEM_BARRIER(HWIO_SE_GENI_TX_FIFO0, buf[i]);
                cache = 0;
            }
            // REG_IN_MEM_BARRIER(HWIO_SE_GENI_M_IRQ_CLR, BIT_SE_GENI_M_IRQ_CLR_M_TX_FIFO_WATERMARK_CLR);
        }
        // Flush remaining data in cache
        if (len & 3)
        {
        //     // geni_poll_bit(HWIO_SE_GENI_M_IRQ_STATUS, BIT_SE_GENI_M_IRQ_STATUS_M_TX_FIFO_WATERMARK, 1);
            REG_IN_MEM_BARRIER(HWIO_SE_GENI_TX_FIFO0, cache);
        //     REG_IN_MEM_BARRIER(HWIO_SE_GENI_M_IRQ_CLR, BIT_SE_GENI_M_IRQ_CLR_M_TX_FIFO_WATERMARK_CLR);
        }
    }

    // Finish poll tx
    // geni_poll_tx_finish();
}
