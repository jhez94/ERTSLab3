#include <exports.h>

#include <arm/psr.h>
#include <arm/exception.h>
#include <arm/interrupt.h>
#include <arm/timer.h>
#include <arm/reg.h>

#include <bits/errno.h>
#include <bits/fileno.h>
#include <bits/swi.h>

#include "globals.h"
#include "swi_handler.h"
#include "irq_handler.h"
#include "user_setup.h"
#include "exit_handler.h"

typedef enum {FALSE, TRUE} bool;
#define bit_isset(val, i) (!(((val) & (1<<(i))) == 0))

// 0xe59ff014 (LDR pc, [pc, 0x14]) --> 0x014 through masking
#define SWI_VECT_ADDR 0x08
#define IRQ_VECT_ADDR 0x18
#define PC_OFFSET 0x08

#define INTERVAL 32500//000
#define OSTMR_0_BIT 0x04000000

// Cannot write to this address. kernel.bin loaded here. Stack grows down.
#define USER_STACK_TOP 0xa3000000

// (LDR pc, [pc, 0x000]) 0xe59ff000 --> 0x000 through masking
#define LDR_PC_PC_INSTR 0xe59ff000
#define LDR_SIGN_MASK 0x00800000

#define BAD_CODE 0x0badc0de

#define SFROM_START 0x00000000
#define SFROM_END 0x00ffffff
#define SDRAM_START 0xa0000000
#define SDRAM_END 0xa3ffffff

/* Checks the SWI Vector Table. */
bool check_exception_vector(int vector_address) {
    int vector_instr = *((int *)vector_address);

    // Check if the offset is negative.
    if ((vector_instr & LDR_SIGN_MASK) == 0) {
        return FALSE;
    }

    // Check that the instruction is a (LDR pc, [pc, 0x000])
    if ((vector_instr & 0xFFFFF000) != LDR_PC_PC_INSTR) {
        return FALSE;
    }

    return TRUE;
}

void wire_handler(int vector_address, void *new_handler, int *oldinstr_1, int *oldinstr_2)
{
    // Jump offset already incorporates PC offset. Usually 0x10 or 0x14.
    int jmp_offset = (*((int *) vector_address))&(0xFFF);

    // &S_Handler" in Jump Table.
    int *handler_addr = *(int **)(vector_address + PC_OFFSET + jmp_offset);

    // Save original Uboot handler instructions.
    *oldinstr_1 = *handler_addr;
    *oldinstr_2 = *(handler_addr + 1);

    // Wire in our own: LDR pc, [pc, #-4] = 0xe51ff004
    *handler_addr = 0xe51ff004;
    *(handler_addr + 1) = (int)new_handler; // New  handler.

    return;

}

void restore_handlers(int vector_address, int instr_1, int instr_2)
{
    // Jump offset already incorporates PC offset. Usually 0x10 or 0x14.
    int jmp_offset = (*((int *) vector_address))&(0xFFF);

    // &Handler in Jump Table.
    int *handler_addr = *(int **)(vector_address + PC_OFFSET + jmp_offset);

    *handler_addr = instr_1;
    *(handler_addr + 1) = instr_2;

    return;
}


/* Verifies that the buffer is entirely in valid memory. */
int check_mem(char *buf, int count, unsigned start, unsigned end) {
    unsigned start_buf = (unsigned) buf;
    unsigned end_buf = (unsigned)(buf + count);

    if ( (start_buf < start) || (start_buf > end) ) {
        return FALSE;
    }
    if ( (end_buf < start) || (end_buf > end) ) {
        return FALSE;
    }
    // Overflow case.
    if ( start_buf >= end_buf ) {
        return FALSE;
    }

    return TRUE;
}

// write function to replace the system's write function
ssize_t write_handler(int fd, const void *buf, size_t count) {

    // Check for invalid memory range or file descriptors
    if (check_mem((char *) buf, (int) count, SDRAM_START, SDRAM_END) == FALSE &&
        check_mem((char *) buf, (int) count, SFROM_START, SFROM_END) == FALSE) {
        exit_handler(-EFAULT);
    } else if (fd != STDOUT_FILENO) {
        exit_handler(-EBADF);
    }

    char *buffer = (char *) buf;
    size_t i;
    char read_char;
    for (i = 0; i < count; i++) {
        // put character into buffer and putc
        read_char = buffer[i];
        putc(read_char);
    }
    return i;
}


// read function to replace the system's read function
ssize_t read_handler(int fd, void *buf, size_t count) {
    // Check for invalid memory range or file descriptors
    if (check_mem((char *) buf, (int) count, SDRAM_START, SDRAM_END) == FALSE) {
        exit_handler(-EFAULT);
    } else if (fd != STDIN_FILENO) {
        exit_handler(-EBADF);
    }

    size_t i = 0;
    char *buffer = (char *) buf;
    char read_char;

    while (i < count) {
        read_char = getc();

        if (read_char == 4) { //EOT character
            return i;
        } else if (((read_char == 8) || (read_char == 127))) { // backspace or DEL character
            buffer[i] = 0; // '\0' character
            if(i > 0) {
                i--;
                puts("\b \b");
            }
        } else if ((read_char == 10) || (read_char == 13)) { // '\n' newline or '\r' carriage return character
            buffer[i] = '\n';
            putc('\n');
            return (i+1);
        } else {
            // put character into buffer and putc
            buffer[i] = read_char;
            i++;
            putc(read_char);
        }
    }

    return i;
}

void time_handler()
{

}

void sleep_handler()
{

}

void C_Timer_0_Handler()
{
    //Todo: Increment a global counter
    /* Reset counter */
    reg_write(OSTMR_OSCR_ADDR, 0);
    /* Acknowledge match */
    reg_set(OSTMR_OSCR_ADDR, OSTMR_OSSR_M0);
}

/* C_SWI_Handler uses SWI number to call the appropriate function. */
int C_SWI_Handler(int swiNum, int *regs) {
    int count = 0;
    printf("C_SWI_Handler ...\n");
    switch (swiNum) {
        // ssize_t read(int fd, void *buf, size_t count);
        case READ_SWI:
            printf("Read SWI Call ...\n");
            count = read_handler(regs[0], (void *) regs[1], (size_t) regs[2]);
            break;
        // ssize_t write(int fd, const void *buf, size_t count);
        case WRITE_SWI:
            printf("Write SWI Call ...\n");
            count = write_handler((int) regs[0], (void *) regs[1], (size_t) regs[2]);
            break;
        // void exit(int status);
        case EXIT_SWI:
            printf("Exit SWI Call ...\n");
            exit_handler((int) regs[0]); // never returns
            break;
        // void sleep(__);
        case SLEEP_SWI:
            printf("Sleep SWI Call ...\n");
            sleep_handler();
            break;
        // void time(__);
        case TIME_SWI:
            printf("Time SWI Call ...\n");
            time_handler();
            break;
        default:
            printf("Error in ref C_SWI_Handler: Invalid SWI number.");
            exit_handler(BAD_CODE); // never returns
    }

    return count;
}

void C_IRQ_Handler() 
{
    uint32_t icpr = reg_read(INT_ICPR_ADDR);

    if(bit_isset(icpr, INT_OSTMR_0))
        C_Timer_0_Handler();

}

uint32_t global_data;
int kmain(int argc, char** argv, uint32_t table)
{
    
    app_startup(); /* Note that app_startup() sets all uninitialized and */ 
            /* zero global variables to zero. Make sure to consider */
            /* any implications on code executed before this. */
    global_data = table;

    /** Wire in the new handlers. **/
    int old_irq_instr_1, old_irq_instr_2, old_swi_instr_1, old_swi_instr_2;
    if (check_exception_vector(SWI_VECT_ADDR) == FALSE)
        return BAD_CODE;
    wire_handler(SWI_VECT_ADDR, &swi_handler, &old_swi_instr_1, &old_swi_instr_2);
    
    if (check_exception_vector(IRQ_VECT_ADDR) == FALSE)
        return BAD_CODE;
    wire_handler(IRQ_VECT_ADDR, &irq_handler, &old_irq_instr_1, &old_irq_instr_2);
    
   
    /* Configure IRQ for timer */
    uint32_t old_iclr = reg_read(INT_ICLR_ADDR);
    reg_clear(INT_ICLR_ADDR, OSTMR_0_BIT); /* Clear bit to generate irq's */
    reg_set(INT_ICMR_ADDR, OSTMR_0_BIT);   /* Set bit to enable irq's */
    //reg_set(INT_ICMR_ADDR, 0x3C000000);   /* Set bit to enable irq's */

    /* Set up timer */    
    reg_write(OSTMR_OSMR_ADDR(0), INTERVAL); /* Set interval */
    reg_set(OSTMR_OIER_ADDR, OSTMR_OIER_E0); /* Enable match 0 */
    reg_write(OSTMR_OSCR_ADDR, 0);           /* Reset counter */

    printf("ICMR  = %x\n", (int)reg_read(INT_ICMR_ADDR));
    printf("OSMR0 = %x\n", (int)reg_read(OSTMR_OSMR_ADDR(0)));
    printf("OSCR  = %x\n", (int)reg_read(OSTMR_OSCR_ADDR));    

    // Copy argc and argv to user stack in the right order.
    int *spTop = ((int *) USER_STACK_TOP) - 1;
    int i = 0;
    for (i = argc-1; i >= 0; i--) {
        *spTop = (int)argv[i];
        spTop--;
    }
    *spTop = argc;

    /** Jump to user program. **/
    int usr_prog_status = user_setup(spTop);


    /** Restore SWI Handler. **/
    restore_handlers(SWI_VECT_ADDR, old_swi_instr_1, old_swi_instr_2);
    restore_handlers(IRQ_VECT_ADDR, old_irq_instr_1, old_irq_instr_2);

    reg_write(INT_ICLR_ADDR, old_iclr);

    return usr_prog_status;
}

