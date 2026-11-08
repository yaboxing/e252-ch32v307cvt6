OUTPUT_ARCH(riscv)
ENTRY(stub_main)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x1000
}

SECTIONS
{
    .text :
    {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
    } >RAM

    /DISCARD/ :
    {
        *(.data)
        *(.data.*)
        *(.bss)
        *(.bss.*)
        *(.comment)
        *(.note*)
        *(.riscv.attributes)
    }
}
