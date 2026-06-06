# Deluge Hardware

Some docs on the hardware/firmware.

## Interrupts

Interrupt Name | Interrupt ID | Handler | Priority | Registered | Called
-- | -- | -- | -- | -- | --
INTC_ID_SPRI0 | 271 | cvSPITransferComplete | 5 | Yes | Yes
INTC_ID_TGI2A | 150 | midiAndGateOutputTimerInterrupt | 5 | Yes | Yes
INTC_ID_DMAINT0 +   PIC_TX_DMA_CHANNEL | 41 + 10 = 51 | PIC_TX_INT_TrnEnd | 5 | Yes | Yes
IRQ_INTERRUPT_0 +   6 | 38 | int_irq6 | 5 | Yes | No? Maybe CV
INTC_ID_USBI0 | 73 | usb_pstd_usb_handler | 9 | Yes | Yes
INTC_ID_SDHI1_0 | 306 | sddev_sd_int_handler_1 | 10 | Yes | Yes
INTC_ID_SDHI1_3 | 305 | sddev_sd_int_handler_1 | 10 | Yes | Yes
INTC_ID_DMAINT0 +   OLED_SPI_DMA_CHANNEL | 41 + 4 = 45 | oledTransferComplete | 13 | Yes | Yes
INTC_ID_DMAINT0 +   MIDI_TX_DMA_CHANNEL | 41 + 11 = 52 | MIDI_TX_INT_TrnEnd | 13 | Yes | Yes
INTC_ID_SDHI1_1 | 307 | sddev_sdio_int_handler_1 | 10 | Yes | No?
INTC_ID_USBI0 | 73 | usb_cpu_usb_int_hand | 9 | No | -
INTC_ID_SDHI0_0 | 303 | sddev_sd_int_handler_0 | 10 | No | -
INTC_ID_SDHI0_3 | 302 | sddev_sd_int_handler_0 | 10 | No | -
INTC_ID_SDHI0_1 | 304 | sddev_sdio_int_handler_0 | 10 | No | -
INTC_ID_USBI0 | 73 | usb_hstd_usb_handler | 9 | Yes | No


## DMA Channels

# Registers and channels used in the code

This includes all named mentions of DMA related registers, theoretically direct address referencing is possible in unknown areas of the code. Channel numbers and define names are always in the same order and used channels are extracted also from function calls.

Register |   | Function | Channel | Channel name
-- | -- | -- | -- | --
N0DA_n | oled.c | oledDMAInit | 4 | OLED_SPI_DMA_CHANNEL
  | uart.c | initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
  | sd_dev_dmacdrv.c | sd_DMAC_PeriReqInit | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
N1DA_n | uart.c | initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
N0SA_n | uart.c | uartFlush | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
  | oled.cpp | OLED::freezeWithError | 4 | OLED_SPI_DMA_CHANNEL
  | oled_low_level.c | oledSelectingComplete | 4 | OLED_SPI_DMA_CHANNEL
  | sd_dev_dmacdrv.c | sd_DMAC_PeriReqInit | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
N1SA_n | uart.c | initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
N0TB_n | uart.c | uartFlush | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
  | oled.cpp | OLED::freezeWithError | 4 | OLED_SPI_DMA_CHANNEL
  | oled_low_level.c | oledSelectingComplete | 4 | OLED_SPI_DMA_CHANNEL
  | sd_dev_dmacdrv.c | sd_DMAC_PeriReqInit | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
CRSA_n | main.c | int_irq6 | 6 | SSI_TX_DMA_CHANNEL
  | ssi.c | getTxBufferCurrentPlace | 6 | SSI_TX_DMA_CHANNEL
  | midi_engine.cpp | brdyOccurred | 6 | SSI_TX_DMA_CHANNEL
  | sio_char.c | midiUartDmaRxTimingLinkDescriptor | 6 | SSI_TX_DMA_CHANNEL
  | r_usb_hreg_abs.c | usb_hstd_interrupt_handler | 6 | SSI_TX_DMA_CHANNEL
  | r_usb_preg_abs.c | usb_pstd_interrupt_handler | 6 | SSI_TX_DMA_CHANNEL
CRDA_n | ssi.c | getRxBufferCurrentPlace | 7 | SSI_RX_DMA_CHANNEL
  | uart.c | uartGetChar | 12 | PIC_RX_DMA_CHANNEL
  | uart.c | uartGetCharWithTiming | 13 | MIDI_RX_DMA_CHANNEL
CRTB_n | sd_dev_dmacdrv.c | sd_DMAC_Close | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
CHSTAT_n | pic.h | waitForFlush | 10 | PIC_TX_DMA_CHANNEL
  | oled.cpp | OLED::freezeWithError | 4 | OLED_SPI_DMA_CHANNEL
  | sd_dev_dmacdrv.c | sd_DMAC_Open   sd_DMAC_Close   sd_DMAC_Get_Endflag | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
CHCTRL_n | dmac.c | dmaChannelStart | 10   12   11   14   13   6   7 | PIC_TX_DMA_CHANNEL   PIC_RX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL   MIDI_RX_TIMING_DMA_CHANNEL   MIDI_RX_DMA_CHANNEL   SSI_TX_DMA_CHANNEL   SSI_RX_DMA_CHANNEL
  | oled.c | oledDMAInit | 4 | OLED_SPI_DMA_CHANNEL
  | uart.c | uartFlushIfNotSending   tx_interrupt   initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
  | oled.cpp | OLED::freezeWithError | 4 | OLED_SPI_DMA_CHANNEL
  | oled_low_level.c | oledSelectingComplete | 4 | OLED_SPI_DMA_CHANNEL
  | sd_dev_dmacdrv.c | sd_DMAC_Open   sd_DMAC_Close | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
CHCFG_n | deluge.cpp | deluge_main | 4 | OLED_SPI_DMA_CHANNEL
  | dmac.c | initDMAWithLinkDescriptor | 6   7   14   12   13 | SSI_TX_DMA_CHANNEL   SSI_RX_DMA_CHANNEL   MIDI_RX_TIMING_DMA_CHANNEL   PIC_RX_DMA_CHANNEL   MIDI_RX_DMA_CHANNEL
  | oled.c | oledDMAInit | 4 | OLED_SPI_DMA_CHANNEL
  | uart.c | uartFlush | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
  |   | initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
  | sd_dev_dmacdrv.c | sd_DMAC_PeriReqInit | 3   2 | SD0_DMA_CHANNEL   SD1_DMA_CHANNEL
CHITVL_n | oled.c | oledDMAInit | 4 | OLED_SPI_DMA_CHANNEL
  | uart.c | initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
CHEXT_n | oled.c | oledDMAInit | 4 | OLED_SPI_DMA_CHANNEL
  | uart.c | initUartDMA | 10   11 | PIC_TX_DMA_CHANNEL   MIDI_TX_DMA_CHANNEL
NXLA_n | dmac.c | initDMAWithLinkDescriptor | 6   7   14   12   13 | SSI_TX_DMA_CHANNEL   SSI_RX_DMA_CHANNEL   MIDI_RX_TIMING_DMA_CHANNEL   PIC_RX_DMA_CHANNEL   MIDI_RX_DMA_CHANNEL

# Channel configuration

Work in progress

## PIC_RX_DMA_CHANNEL (12)

```
Config:
CHCFG_n = 0b10000001000100000000000001100000 | DMA_AM_FOR_SCIF | (PIC_RX_DMA_CHANNEL & 7
	• Default values:
		○ REQD = 0: Source; DMAACK is to become active when read
		○ LOEN = 0: Does not detect a request even when the signal is at the Low level (initial value).
		○ TM = 0: Single transfer mode
		○ SBE = 0: no sweep
		○ RSEL = 0: Executes the Next0 register set (inverted with RSW) (only register mode)
		○ RSW = 0: Does not invert RSEL automatically after a DMA transaction (only register mode).
		○ REN = 0: Does not continue DMA transfers (only register mode)
	• ACK Mode (10 to 8) = 01x: Bus cycle mode (active while the DMA transfer is in a bus cycle)
	• HIEN = 1: Detects a request when the signal is at the High level
	• LVL = 1: Detects based on the level.
	• SDS and DDS = 0000: 8 bit source and destination transfer size
	• SAD = 1: Fixed source address
	• DAD = 0: Incrementing destination address
	• DEM = 1: Masks the DMA transfer end interrupt
	• DMS = 1: Link mode

On startup:
CHCTRL_n |= DMAC_CHCTRL_0S_SWRST: Reset channel status register
CHCTRL_n |= DMAC_CHCTRL_0S_SETEN: Enable DMA transfer on this channel

Link configuration:
NXLA_n = (uint32_t)linkDescriptor
	• Header = 0b1101
		○ bit 0 = 1: Descriptor valid
		○ bit 1 = 0: The link continues
		○ bit 2 = 1: Does not write back the LV bit
		○ bit 3 = 1: Does not issue a DMA transfer end interrupt
	• SRC = SCIFA(UART_CHANNEL_PIC).SCFRDR
	• DST = picRxBuffer
	• SIZE = PIC_RX_BUFFER_SIZE (64)
	• Config = (see above)
	• Interval = 0
	• Extension = 0
	• Next address = self
```

