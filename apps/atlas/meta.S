/* meta.S — Signal Atlas .m1meta manifest */
    .syntax unified
    .thumb

    .section .m1meta, "a", %progbits
    .align 2
    .global m1_app_manifest
m1_app_manifest:
    .word 0x4D314150          /* magic "M1AP" */
    .short 9                  /* api_version */
    .short 4096               /* stack_size: 4096 words = 16KB (scan buffers) */
    .ascii "Signal Atlas\0"   /* name[32] */
    .zero 20                  /* pad name to 32 bytes */
    .ascii "Recon\0"          /* category */
    .zero 11                  /* pad category to 16 bytes */
    .ascii "1.0.0\0"          /* version */
    .zero 10                  /* pad version to 16 bytes */
