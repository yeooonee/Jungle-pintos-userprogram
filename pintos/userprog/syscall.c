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
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include <string.h>
#include "include/threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/off_t.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);
struct lock file_lock;

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
	
	lock_init(&file_lock);
}



static void
halt (void) {
	power_off ();
}

void
exit (int status) {
	struct thread *curr = thread_current ();
	curr->exit_status = status;
	thread_exit ();
}

static void
range_chk (void *args, void *end_args){
	if ((args == NULL) || !is_user_vaddr(args) || !pml4_get_page(thread_current()->pml4, args)){
		exit(-1);
	}
	if (end_args != NULL){
		for (uintptr_t i = (uintptr_t) pg_round_down(args); i <= end_args - 1; i += PGSIZE){
			if (!is_user_vaddr(i) || !pml4_get_page(thread_current()->pml4, (const void *) i)){
				exit(-1);
			}
		}
	}
}

static pid_t
fork (const char *thread_name, struct intr_frame *f){
	// 1) thread_name 주소 검사
	range_chk(thread_name, NULL);

	// 2) process_fork 호출 및 반환
	return process_fork(thread_name, f);

}

static int
exec (const char *file) {
	range_chk(file, NULL);

	// /* 파일 이름 보존 위해 palloc 으로 할당 (process_cleanup 에서 사라지지 않게 하기 위해)*/
	// char *file_name = palloc_get_page(0);
	// memcpy(file_name, file, strlen(file) + 1);

	int rtn = process_exec((void *) file);
	// palloc_free_page(file_name);
	
	if (rtn == -1) {
		exit(-1);
	}
}

static int
wait (pid_t pid) {
	return process_wait(pid);
}

static bool
create (const char *file, unsigned initial_size) {
	range_chk(file, NULL);					//# 포인터 유효성 검증
	lock_acquire(&file_lock);				//# 파일시스템 락 설정

	bool result = filesys_create(file, initial_size);  //# 파일 생성
	lock_release(&file_lock);
	return result;
}

static bool
remove (const char *file) {
	range_chk(file, NULL);
	lock_acquire(&file_lock);
	bool result = filesys_remove(file);
	lock_release(&file_lock);
	return result;
}

static int
open (const char *file) {
	range_chk(file, NULL);

	// 파일 오픈
	lock_acquire(&file_lock);
	struct file *file_rtn = filesys_open(file);
	lock_release(&file_lock);

	if (file_rtn == NULL) return -1;

	// 파일 fd 테이블에 저장하기
	struct file **curr_file_p = thread_current()->file_p;

	for (int i = FD_START; i < FD_MAX; i++){
		if (curr_file_p[i] == NULL) {
			curr_file_p[i] = file_rtn;
			return i;
		}
	}
	
	lock_acquire(&file_lock);
	file_close(file_rtn);
	lock_release(&file_lock);

	return -1;

}

static int
filesize (int fd) {
	struct thread *t = thread_current();

	if (fd < FD_START || fd >= FD_MAX || t->file_p[fd] == NULL) {
		return -1;
	}
	lock_acquire(&file_lock);
	int length = file_length(t->file_p[fd]);
	lock_release(&file_lock);

	return length;
}

static int
read (int fd, void *buffer, unsigned size) {
	struct thread *curr = thread_current();

	//# buffer 포인터 유효성 검증
	if (size == 0) {
		return 0;
	}
	range_chk(buffer, buffer + size);

	//# fd0 키보드에서 읽기
	if (fd == 0) {
		for (unsigned i = 0; i < size; i++) {
			((char *)buffer)[i] = input_getc();
		}
		return size;
	} 
	
	//# fd1 읽기 대상이 아니므로 -1 반환
	if (fd == 1) {
		return -1;
	}

	//# fd가 유효한 범위인지 확인
	if (fd < FD_START || fd >= FD_MAX) {
		return -1;
	}
	
	//# fd2 : 파일에서 읽기
	struct file *f = curr->file_p[fd];
	if (f == NULL) {
		return -1;
	}
	lock_acquire(&file_lock);				//# 파일시스템 락 설정
	int result = file_read(f, buffer, size);
	lock_release(&file_lock);
	return result;
}

static int
write (int fd, const void *buffer, unsigned size) {
	range_chk(buffer, buffer + size);

	if (fd == 1) {
		putbuf (buffer, size);
		return size;
	} 

	if (FD_START > fd || fd >= FD_MAX) return -1;

	struct file *curr_file_p = thread_current()->file_p[fd];
	if (curr_file_p == NULL) return -1;

	lock_acquire(&file_lock);
	off_t t = file_write(curr_file_p, buffer, size);
	lock_release(&file_lock);
	
	return t;

}

static void
seek (int fd, unsigned position) {
	struct thread *t = thread_current();
	if (fd < FD_START || fd >= FD_MAX || t->file_p[fd] == NULL) {
		return;
	}

	lock_acquire(&file_lock);
	file_seek(t->file_p[fd], position);
	lock_release(&file_lock);
}

static unsigned
tell (int fd) {
	struct thread *t = thread_current();
	if (fd <2 || fd >= 128 || t->file_p[fd] == NULL){
		return -1;
	}

	lock_acquire(&file_lock);
	unsigned re = file_tell(t->file_p[fd]);
	lock_release(&file_lock);

	return re;
}

static void
close (int fd) {
	struct thread *t = thread_current();
	if (fd < FD_START || fd >= FD_MAX || t->file_p[fd] == NULL) {
		return;
	}
	lock_acquire(&file_lock);
	file_close(t->file_p[fd]);
	lock_release(&file_lock);
	t->file_p[fd] = NULL;
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

