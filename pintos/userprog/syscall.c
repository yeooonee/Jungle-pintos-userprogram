#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "kernel/stdio.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include <string.h>

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

typedef int pid_t;
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}



void
halt (void) {
	power_off ();
}

void
exit (int status) {
	struct thread *curr = thread_current ();
	curr->exit_status = status;
	thread_exit ();
}

pid_t
fork (const char *thread_name, struct intr_frame *f){
	// 1) thread_name 주소 검사
	if ((thread_name == NULL) || !is_user_vaddr(thread_name) || !pml4_get_page(thread_current()->pml4, thread_name)){
		exit(-1);
	}

	// 2) process_fork 호출 및 반환
	return process_fork(thread_name, f);

}

int
exec (const char *file) {
	if (file == NULL || !is_user_vaddr(file) || !pml4_get_page(thread_current()->pml4, file)){
		exit(-1);
	}

	/* 파일 이름 보존 위해 palloc 으로 할당 (process_cleanup 에서 사라지지 않게 하기 위해)*/
	char *file_name = palloc_get_page(0);
	memcpy(file_name, file, strlen(file) + 1);

	int rtn = process_exec(file_name);
	palloc_free_page(file_name);
	
	if (rtn == -1) {
		exit(-1);
	}
}

int
wait (pid_t pid) {
	return process_wait(pid);
}

bool
create (const char *file, unsigned initial_size) {
}

bool
remove (const char *file) {
}

int
open (const char *file) {
}

int
filesize (int fd) {
}

int
read (int fd, void *buffer, unsigned size) {
}

int
write (int fd, const void *buffer, unsigned size) {
	if (fd == 1) {
		putbuf (buffer, size);
		return size;
	}




	// else {
	// 	struct file *f = 
	// 	return file_write();
	// }
	

}

void
seek (int fd, unsigned position) {
}

unsigned
tell (int fd) {
}

void
close (int fd) {
}



/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	int syscall_num = f->R.rax;

	switch (syscall_num) {
		case SYS_HALT:
			halt ();
			break;
		case SYS_EXIT:
			exit (f->R.rdi);
			break;
		case SYS_FORK:
			f->R.rax = fork ((const char *)f->R.rdi, f);
			break;
		case SYS_EXEC:
			f->R.rax = exec ((const char *)f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = wait (f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = create ((const char *)f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove ((const char *)f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = open ((const char *)f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize (f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = read (f->R.rdi, (void *)f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write (f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			seek (f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell (f->R.rdi);
			break;
		case SYS_CLOSE:
			close (f->R.rdi);
			break;
	}
}

