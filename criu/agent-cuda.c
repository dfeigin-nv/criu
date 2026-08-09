#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <common/list.h>
#include <compel/infect.h>

#include "agent-cuda.h"
#include "criu-log.h"
#include "types.h"
#include "proc_parse.h"
#include "pstree.h"
#include "util.h"

#define AGENT_CUDA_VPIDS_ENV "CRIU_AGENT_CUDA_VPIDS"
#define CUDA_CHECKPOINT_HELPER "/usr/local/bin/cuda-checkpoint-helper"

struct agent_cuda_task {
	int restore_tid;
	k_rtsigset_t restore_sigset;
	struct list_head list;
};

static LIST_HEAD(agent_cuda_tasks);
static bool agent_cuda_enabled;

static void agent_cuda_free_tasks(void)
{
	struct agent_cuda_task *task;
	struct agent_cuda_task *next;

	list_for_each_entry_safe(task, next, &agent_cuda_tasks, list) {
		list_del(&task->list);
		xfree(task);
	}
	agent_cuda_enabled = false;
}

static int helper_restore_tid(int pid)
{
	char pid_buf[16];
	char output[64] = {};
	char *end;
	int fd[2];
	pid_t child;
	int status;
	ssize_t read_size;
	long tid;

	if (pipe(fd)) {
		pr_perror("create CUDA restore-tid pipe");
		return -1;
	}
	child = fork();
	if (child < 0) {
		pr_perror("fork CUDA restore-tid helper");
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (child == 0) {
		snprintf(pid_buf, sizeof(pid_buf), "%d", pid);
		if (dup2(fd[1], STDOUT_FILENO) < 0)
			_exit(EXIT_FAILURE);
		close(fd[0]);
		close(fd[1]);
		execl(CUDA_CHECKPOINT_HELPER, CUDA_CHECKPOINT_HELPER, "--get-restore-tid", "--pid", pid_buf, NULL);
		_exit(EXIT_FAILURE);
	}

	close(fd[1]);
	read_size = read(fd[0], output, sizeof(output) - 1);
	close(fd[0]);
	if (read_size < 0) {
		pr_perror("read CUDA restore-tid helper output");
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return -1;
	}
	if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
		pr_err("CUDA restore-tid helper failed for pid %d\n", pid);
		return -1;
	}
	output[read_size] = '\0';
	errno = 0;
	tid = strtol(output, &end, 10);
	while (isspace(*end))
		end++;
	if (errno || end == output || *end || tid <= 0 || tid > INT_MAX) {
		pr_err("invalid CUDA restore tid %s for pid %d\n", output, pid);
		return -1;
	}
	return (int)tid;
}

static int resume_restore_thread(struct agent_cuda_task *task)
{
	k_rtsigset_t block;

	if (ptrace(PTRACE_GETSIGMASK, task->restore_tid, sizeof(task->restore_sigset), &task->restore_sigset)) {
		pr_perror("get signal mask for CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	ksigfillset(&block);
	ksigdelset(&block, SIGTRAP);
	if (ptrace(PTRACE_SETSIGMASK, task->restore_tid, sizeof(block), &block)) {
		pr_perror("block signals for CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	if (ptrace(PTRACE_SETOPTIONS, task->restore_tid, NULL, 0)) {
		pr_perror("clear ptrace options for CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	if (ptrace(PTRACE_CONT, task->restore_tid, NULL, 0)) {
		pr_perror("resume CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	return 0;
}

static int interrupt_restore_thread(struct agent_cuda_task *task)
{
	struct proc_status_creds creds;

	if (ptrace(PTRACE_INTERRUPT, task->restore_tid, NULL, 0)) {
		pr_perror("interrupt CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	if (compel_wait_task(task->restore_tid, -1, parse_pid_status, NULL, &creds.s, NULL) != COMPEL_TASK_ALIVE) {
		pr_err("wait for CUDA restore tid %d after interrupt\n", task->restore_tid);
		return -1;
	}
	if (ptrace(PTRACE_SETOPTIONS, task->restore_tid, NULL, PTRACE_O_SUSPEND_SECCOMP | PTRACE_O_TRACESYSGOOD)) {
		pr_perror("restore ptrace options for CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	if (ptrace(PTRACE_SETSIGMASK, task->restore_tid, sizeof(task->restore_sigset), &task->restore_sigset)) {
		pr_perror("restore signal mask for CUDA restore tid %d", task->restore_tid);
		return -1;
	}
	return 0;
}

static int add_virtual_pid(const char *value)
{
	char *end;
	long virtual_pid;
	struct pstree_item *item;
	struct agent_cuda_task *task;

	errno = 0;
	virtual_pid = strtol(value, &end, 10);
	if (errno || end == value || *end || virtual_pid <= 0 || virtual_pid > INT_MAX) {
		pr_err("invalid CRIU_AGENT_CUDA_VPIDS entry %s\n", value);
		return -1;
	}
	item = pstree_item_by_virt((pid_t)virtual_pid);
	if (!item || !task_alive(item)) {
		pr_err("CUDA virtual pid %ld is not an alive restored task\n", virtual_pid);
		return -1;
	}
	task = xzalloc(sizeof(*task));
	if (!task)
		return -1;
	task->restore_tid = helper_restore_tid(item->pid->real);
	if (task->restore_tid < 0) {
		xfree(task);
		return -1;
	}
	if (resume_restore_thread(task)) {
		xfree(task);
		return -1;
	}
	list_add_tail(&task->list, &agent_cuda_tasks);
	pr_info("Agent CUDA bridge resumed restore tid %d for virtual pid %ld\n", task->restore_tid, virtual_pid);
	return 0;
}

int agent_cuda_prepare(void)
{
	char *copy;
	char *entry;
	char *save;
	const char *configured = getenv(AGENT_CUDA_VPIDS_ENV);

	if (!configured || !configured[0])
		return 0;
	copy = xstrdup(configured);
	if (!copy)
		return -1;
	for (entry = strtok_r(copy, ",", &save); entry; entry = strtok_r(NULL, ",", &save)) {
		if (add_virtual_pid(entry)) {
			xfree(copy);
			agent_cuda_finish();
			return -1;
		}
	}
	xfree(copy);
	if (list_empty(&agent_cuda_tasks)) {
		pr_err("CRIU_AGENT_CUDA_VPIDS contains no CUDA processes\n");
		return -1;
	}
	agent_cuda_enabled = true;
	return 0;
}

bool agent_cuda_requested(void)
{
	return agent_cuda_enabled;
}

int agent_cuda_finish(void)
{
	struct agent_cuda_task *task;
	int ret = 0;

	list_for_each_entry(task, &agent_cuda_tasks, list) {
		if (interrupt_restore_thread(task))
			ret = -1;
	}
	agent_cuda_free_tasks();
	return ret;
}
