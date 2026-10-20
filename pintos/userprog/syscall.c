#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "kernel/stdio.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}



//# 하는 일 : 
void
halt (void) {
	power_off ();
}


//# 하는 일
//# 동작 순서 : exit -> thread_exit -> process_exit
void
exit (int status) {
	struct thread* curr = thread_current();

	//# exit status 저장. process_exit에서 child_info에 넘겨줄 때 씀
	if (curr->child_info != NULL) {
		curr->child_info->exit_status = status;
	}

	//# 내부에서 process_exit() 호출
	thread_exit();	
}

pid_t
fork (const char *thread_name){
}

int
exec (const char *file) {

}

//# 하는 일
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
			f->R.rax = fork ((const char *)f->R.rdi);
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

