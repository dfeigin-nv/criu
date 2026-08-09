#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "extmem.h"
#include "images/extmem.pb-c.h"
#include "memfd.h"

static int provider_fd = -1;
static int provider_lookup_count;

int inherit_fd_lookup_id(char *id)
{
	assert(strcmp(id, "0-extmem-provider") == 0);
	provider_lookup_count++;
	return dup(provider_fd);
}

static void send_response(int socket, int status, int fd)
{
	ExtmemResp response = EXTMEM_RESP__INIT;
	uint8_t buffer[128];
	struct iovec iov = { .iov_base = buffer };
	struct msghdr message = {};
	char control[CMSG_SPACE(sizeof(int))] = {};
	size_t length;

	response.status = status;
	length = extmem_resp__pack(&response, buffer);
	iov.iov_len = length;
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	if (fd >= 0) {
		struct cmsghdr *cmsg;

		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	}
	assert(sendmsg(socket, &message, 0) == (ssize_t)length);
}

static void run_provider(int socket)
{
	uint8_t buffer[256];
	int request_number;

	for (request_number = 0; request_number < 5; request_number++) {
		ExtmemReq *request;
		ssize_t length = recv(socket, buffer, sizeof(buffer), 0);

		assert(length > 0);
		request = extmem_req__unpack(NULL, length, buffer);
		assert(request);
		if (request_number == 0) {
			assert(request->op == EXTMEM_OP__EXTMEM_INIT);
			send_response(socket, 0, -1);
		} else if (request_number == 1) {
			int image_fd;

			assert(request->op == EXTMEM_OP__EXTMEM_OPEN_IMAGE);
			image_fd = memfd_create("extmem-test", 0);
			assert(image_fd >= 0);
			send_response(socket, 0, image_fd);
			close(image_fd);
		} else if (request_number == 2) {
			int vma_fd;

			assert(request->op == EXTMEM_OP__EXTMEM_GET_VMA);
			assert(request->get_vma);
			assert(request->get_vma->pid == 123);
			assert(request->get_vma->vma_id == 7);
			assert(request->get_vma->vaddr == 0x1000);
			assert(request->get_vma->length == 0x2000);
			vma_fd = memfd_create("extmem-vma-test", 0);
			assert(vma_fd >= 0);
			send_response(socket, 0, vma_fd);
			close(vma_fd);
		} else if (request_number == 3) {
			int shared_fd;

			assert(request->op == EXTMEM_OP__EXTMEM_GET_SHARED);
			assert(request->get_shared);
			assert(request->get_shared->shmid == 55);
			assert(request->get_shared->length == 0x3000);
			shared_fd = memfd_create("extmem-shared-test", MFD_ALLOW_SEALING);
			assert(shared_fd >= 0);
			assert(ftruncate(shared_fd, 0x3000) == 0);
			send_response(socket, 0, shared_fd);
			close(shared_fd);
		} else {
			assert(request->op == EXTMEM_OP__EXTMEM_ABORT);
			send_response(socket, 0, -1);
		}
		extmem_req__free_unpacked(request, NULL);
	}
	close(socket);
}

void test_extmem(void)
{
	int sockets[2], image_fd, shared_fd, status;
	pid_t pid;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		close(sockets[0]);
		run_provider(sockets[1]);
		_exit(0);
	}
	close(sockets[1]);
	provider_fd = sockets[0];
	assert(extmem_start_batch() == 0);
	assert(extmem_open_image("pages-1", O_RDONLY, &image_fd) == 0);
	assert(image_fd >= 0);
	close(image_fd);
	assert(extmem_get_vma(123, 7, 0x1000, 0x2000, &image_fd) == 0);
	assert(image_fd >= 0);
	close(image_fd);
	assert(provider_lookup_count == 1);
	extmem_finish_batch();
	assert(extmem_get_shared(55, 0x3000, &shared_fd) == 0);
	assert(shared_fd >= 0);
	assert(extmem_validate_memfd(shared_fd, 0x3000) == 0);
	close(shared_fd);
	/* Repeated mappings of the same shmid reuse the provider object. */
	assert(extmem_get_shared(55, 0x3000, &shared_fd) == 0);
	assert(shared_fd >= 0);
	close(shared_fd);
	assert(extmem_abort() == 0);
	assert(provider_lookup_count == 3);
	close(sockets[0]);
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
