ENTRY(Main)

SECTIONS
{
    . = 0;
    .text :
    {
        KEEP(*(.text.Main))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    __fixer_code_end = .;

    . = ALIGN(0x1000);
    __fixer_data_start = .;
    .data :
    {
        *(.data .data.*)
    }
    .bss (NOLOAD) :
    {
        *(.bss .bss.* COMMON)
    }
    __fixer_data_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}

ASSERT(Main == 0, "3NR fixer Main must be the first code address")
ASSERT(__fixer_data_start == ALIGN(__fixer_code_end, 0x1000), "3NR fixer data must follow code at the next page")