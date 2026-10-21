#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif
#include "threads/malloc.h"



#define THREAD_NAME_MAX  16 //# Thread 이름은 최대 14 캐릭터

extern struct lock file_lock;

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

struct task_args {
	char *fn_copy;
	struct child_status *cs;
};

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	struct child_status *cs = malloc(sizeof *cs);	
	struct task_args *args = malloc(sizeof *args);
	
	if (cs == NULL) {
		palloc_free_page (fn_copy);
		free(args);
		return TID_ERROR;
	}else if (args == NULL){
		free(cs);
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}
	
	sema_init(&cs->wait_sema, 0);
	sema_init(&cs->fork_sema, 0);
	cs->exit_status = -1;

	args->cs = cs;
	args->fn_copy = fn_copy;

	char file_name_cpy[THREAD_NAME_MAX];
	strlcpy (file_name_cpy, file_name, THREAD_NAME_MAX);
	char *file_name_tkn, *save_ptr;
	file_name_tkn = strtok_r (file_name_cpy, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name_tkn, PRI_DEFAULT, initd, args);
	if (tid == TID_ERROR){
		palloc_free_page (fn_copy);
		free(cs);
		free(args);
		return TID_ERROR;
	}
	cs->child_pid = tid;
	struct thread *curr = thread_current();
	list_push_back (&curr->child_list, &cs->child_elem);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
	struct task_args *args = (struct task_args *) f_name;
	char *fn_copy = args->fn_copy;
	struct child_status *cs = args->cs;

	thread_current()->self_status = cs;

	free(args);


#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (fn_copy) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}



struct job_args {
	struct thread *th;
	struct intr_frame *itrf;
	struct child_status *cs;
	bool result_chk;

};

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *curr = thread_current();
	
	// malloc 불러오기
	struct child_status *cs = malloc(sizeof *cs);	
	if (cs == NULL) return TID_ERROR;
	sema_init(&cs->wait_sema, 0);
	sema_init(&cs->fork_sema, 0);
	cs->exit_status = -1;
	cs->fork_success = false;	

	struct job_args *ja = malloc(sizeof *ja);
	if (ja == NULL) {
		free(cs);
		return TID_ERROR;
	}

	// thread_create aux 전달 값 묶음으로 추가 
	// TODO 어떤 식으로 aux 넘길지 (if_, current_thread)
	ja->itrf = if_;
	ja->th = curr;
	ja->cs = cs;
	ja->result_chk = false;
	
	// 현재 프로세스 복제한 새 프로세스 생성
	/* Clone current thread to new thread.*/
	tid_t tid = thread_create (name,
			PRI_DEFAULT, __do_fork, ja);
			
	// thread_create 성공 여부 확인
	if (tid == TID_ERROR){
		free(ja);
		free(cs);
		return TID_ERROR;
	} 

	sema_down(&cs->fork_sema);
	bool fork_ok = ja->result_chk;
	free(ja);
	
	if (!fork_ok) {
		free(cs);
		return TID_ERROR;
	}
	
	// child_status 생성
	cs->child_pid = tid;
	
	// 반환값 부모의 thread child_list 에 저장
	list_push_back(&curr->child_list, &cs->child_elem);

	return tid;

}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va)) {
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL) {		//# parent_page가 NULL인 경우 처리
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	if ((newpage = palloc_get_page(PAL_USER)) == NULL) {
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte); //# 자식 페이지의 쓰기 권한 결정


	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	
	struct job_args *ja = (struct job_args *) aux;
	struct child_status *cs = ja->cs;
	struct thread *parent = ja->th;
	
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = ja->itrf;
	bool succ = true;

	// 자식 thread 에 본인 cs 기록 (종료할 때 필요)
	current->self_status = cs;
	
	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	// 파일 복사 
	for (int i = FD_START; i < FD_MAX; i++){
		if (parent->file_p[i] == NULL)
			continue;
		current->file_p[i] = file_duplicate(parent->file_p[i]);
		if (current->file_p[i] == NULL) goto error;
	}

	process_init ();

	ja->result_chk = true;

	// sema_up(&parent->child_status->fork_sema);
	sema_up(&cs->fork_sema);

	// if_ 의 rax 0으로
	if_.R.rax = 0;
	
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
	
error:
	current->self_status = NULL;
	sema_up(&cs->fork_sema);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = palloc_get_page(0);
	if (file_name == NULL) {
		return -1;
	}
	strlcpy(file_name, f_name, PGSIZE);

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	bool success = load (file_name, &_if);
	palloc_free_page(file_name);

	/* If load failed, quit. */
	if (!success)
		exit(-1);

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */

	// 현제 스레드를 불러옴
	// child_status child 변수를 NULL로 선언
	// list_elem e를 선언
	struct thread *t = thread_current();
	struct child_status *child = NULL;
	struct list_elem *e;
	

	//for child_list를 탐색
	for (e = list_begin(&t->child_list); e != list_end(&t->child_list); e = list_next(e)) {
		struct child_status *cs = list_entry(e, struct child_status, child_elem);
		if (child_tid == cs->child_pid) {
			child = cs;
			break;
		}
	}
	
	// 내 자식아니거나 | 리스트에 없다
	if (child == NULL) {
		// 예외 return -1;
		return -1;
	}
		
	// sema_down(부모)
	sema_down(&child->wait_sema);
	
	// 자식 프로세스의 종료코드 반환값을 명시적으로 선언
	int exit_status = child->exit_status;

	// list에서 지워주고
	list_remove(&child->child_elem);

	// free
	free(child);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	// Process Termination Message
	if (curr->pml4 != NULL)
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
	
	//# fd_table 정리
	if (curr->file_p != NULL) {
		for (int i = FD_START; i < FD_MAX; i++) {
			if (curr->file_p[i] != NULL) {
				file_close(curr->file_p[i]);
				curr->file_p[i] = NULL;
			}
		}
		palloc_free_page(curr->file_p);
		curr->file_p = NULL;
	}

	process_cleanup ();

	// TDOO sema_up 부모 스레드
	if(curr->self_status != NULL){
		// 자식 사라지기 전에 exit status 기록
		curr->self_status->exit_status = curr->exit_status;
		// 기다리던 부모 깨우기
		sema_up(&curr->self_status->wait_sema);
	}
}
	
/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	if (curr->file_run != NULL) {
			// 실행파일 정리
		lock_acquire(&file_lock);
		file_close(curr->file_run);
		lock_release(&file_lock);
		curr->file_run = NULL;
	}

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* file_name_cpy 함수 상단에서 초기화 */
	/* 바로 아래 if문에서 done으로 점프했을 때 file_name_cpy가 선언되어 있어야함 */
	// char *가 직관적이긴 하지만 void *로 해도 상관없다고함
	void *file_name_cpy = NULL;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	//# Argument Passing 구현 
	char *token, *save_ptr;
	char *argv[64];
	int argc = 0;
	int len;
	char *address[64];

	//# Argument Passing 1 : 명령어 파싱
	// 참고 : file_name 변수는 const. 복사 안해도 작동은 하지만 하는게 더 클린함
	if ((file_name_cpy = palloc_get_page(0)) == NULL) { goto done; }

	memcpy(file_name_cpy, file_name, strlen(file_name) + 1);
	for (token = strtok_r (file_name_cpy, " ", &save_ptr); token != NULL; 
		token = strtok_r (NULL, " ", &save_ptr)) {
		argv[argc] = token;
		argc++;
	}


	/* Open executable file. */
	lock_acquire(&file_lock);
	file = filesys_open (argv[0]); //# file_name에서 argv[0]로 수정
	lock_release(&file_lock);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))		//# 스택 초기 메모리 세팅
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;	//# 프로그램 진입점 세팅

	//# Argument Passing 2 : 파싱한 문자열 데이터를 스택 적재
	for (int i = argc - 1; i >= 0; i--) {
		len = strlen(argv[i]) + 1;
		if_->rsp -= len;
		memcpy((void*)if_->rsp, (void*)argv[i], len);
		address[i] = (char*)if_->rsp;
	}

	//# Argument Passing 3 : 패딩 (스택 포인터를 8의 배수로 하기 위해 마지막 3비트 제거)
	if_->rsp = (if_->rsp) & ~0x7;
	


	//# Argument Passing 4 : null terminator 추가 (포인터 배열 끝임을 표시)
	if_->rsp -= sizeof(char*);
	*(char**)if_->rsp = NULL;


	//# Argument Passing 5 : 문자열이 어디 있는지 알려주는 포인터를 스택 적재
	for (int i = argc - 1; i >= 0; i--) {
		if_->rsp -= sizeof(uint64_t);
		*(char**)if_->rsp = address[i];
	}

	//# Argument Passing 6 : rdi, rsi 레지스터 저장
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp;

	//# Argument Passing 7 : 반환주소 0 스택 적재
	if_->rsp -= sizeof(void*);
	*(void**)if_->rsp = NULL;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (success){
		lock_acquire(&file_lock);
		file_deny_write(file);	// 쓰기 금지
		lock_release(&file_lock);
		t->file_run = file;
	} else {
		lock_acquire(&file_lock);
		file_close (file);
		lock_release(&file_lock);
	}
	if (file_name_cpy != NULL) {palloc_free_page(file_name_cpy); }
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
