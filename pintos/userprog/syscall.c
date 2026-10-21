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




static void
check_address (const void *addr) {
	if (addr == NULL || !is_user_vaddr(addr) || !pml4_get_page(thread_current()->pml4, addr)){
		exit(-1);
	}
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

static pid_t
fork (const char *thread_name, struct intr_frame *f){
	// 1) thread_name 주소 검사
	if ((thread_name == NULL) || !is_user_vaddr(thread_name) || !pml4_get_page(thread_current()->pml4, thread_name)){
		exit(-1);
	}

	// 2) process_fork 호출 및 반환
	return process_fork(thread_name, f);

}

static int
exec (const char *file) {
	check_address(file);

	/* 파일 이름 보존 위해 palloc 으로 할당 (process_cleanup 에서 사라지지 않게 하기 위해)*/
	char *file_name = palloc_get_page(0);
	memcpy(file_name, file, strlen(file) + 1);

	int rtn = process_exec(file_name);
	palloc_free_page(file_name);
	
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
	check_address(file);						//# 포인터 유효성 검증
	lock_acquire(&filesys_lock);				//# 파일시스템 락 설정

	bool result = filesys_create(file, initial_size);  //# 파일 생성
	lock_release(&filesys_lock);
	return result;
}

static bool
remove (const char *file) {
	check_address(file);
	lock_acquire(&filesys_lock);



}

static int
open (const char *file) {
	check_address(file);					//# 포인터 유효성 검증
	lock_acquire(&filesys_lock);			//# 파일시스템 락 설정

	struct file *f = filesys_open(file);	//# 파일 열기
	lock_release(&filesys_lock);

	if (f == NULL) {						//# 파일이 없거나, 열기 실패했는지 확인
		return -1;
	}
	struct thread *curr = thread_current();
	int fd = -1;
	for (int i = 3; i < MAX_FD; i++) {
		if (curr->fd_table[i] == NULL) {
			fd = i;
			break;
		}
	}

	if (fd == -1) {			//# fd_table이 꽉찼으면 파일 닫고 -1 리턴
		file_close(f);
		return -1;
	}
	curr->fd_table[fd] = f;		//# fd번 인덱스에 파일 포인터 저장
	return fd;
}

static int
filesize (int fd) {
}

static int
read (int fd, void *buffer, unsigned size) {
	struct thread *curr = thread_current();

	//# buffer 포인터 유효성 검증
	if (size == 0) {
		return 0;
	}
	check_address(buffer);
	check_address(buffer + size - 1);

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
	if (fd < 2 || fd >= MAX_FD) {
		return -1;
	}
	
	//# fd2 : 파일에서 읽기
	struct file *f = curr->fd_table[fd];
	if (f == NULL) {
		return -1;
	}
	lock_acquire(&filesys_lock);				//# 파일시스템 락 설정
	int result = file_read(f, buffer, size);
	lock_release(&filesys_lock);
	return result;
}

static int
write (int fd, const void *buffer, unsigned size) {
	struct thread *curr = thread_current();

	//# buffer 포인터 유효성 검증
	check_address(buffer);
	check_address(buffer + size - 1);

	//# fd1 : 화면에 출력
	if (fd == 1) {
		putbuf (buffer, size);
		return size;
	} 

	//# fd가 유효한 범위인지 확인
	if (fd < 3 || fd >= MAX_FD) {
		return -1;
	}
	
	//# fd2 : 파일에 쓰기
	struct file *f = curr->fd_table[fd];
	if (f == NULL) {
		return -1;
	}
	lock_acquire(&filesys_lock);				//# 파일시스템 락 설정
	int result = file_write(f, buffer, size);
	lock_release(&filesys_lock);
	return result;
}

static void
seek (int fd, unsigned position) {
}

static unsigned
tell (int fd) {
}

static void
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

