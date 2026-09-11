ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0xC000, LENGTH = 0x45BA8
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x2E000
}
SECTIONS {
 .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
 .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
 .ARM.extab : { *(.ARM.extab*) } > FLASH
 .ARM.exidx : { *(.ARM.exidx*) } > FLASH
 .data : { . = ALIGN(4); __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT> FLASH
 __data_load = LOADADDR(.data);
 __image_end = LOADADDR(.data) + SIZEOF(.data);
 .bss (NOLOAD) : { . = ALIGN(64); __bss_start = .; *(.bss*) *(COMMON) *(.m_usb_global*) *(.m_usb_dma_init_data*) *(.m_usb_dma_noninit_data*) . = ALIGN(4); __bss_end = .; } > RAM
 /DISCARD/ : { *(.comment) *(.note*) }
 ASSERT(__image_end <= 0x51BA8, "MCU1 image crosses CRC boundary")
 ASSERT(__bss_end <= 0x2002E000, "MCU1 RAM overlaps 8 KiB stack")
}
