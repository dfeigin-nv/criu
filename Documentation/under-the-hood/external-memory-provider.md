# External memory provider

The optional external memory provider is a restore-time source for CRIU image
files and memory-backed objects. It is enabled by passing a connected Unix
`SOCK_SEQPACKET` descriptor as the inherited-FD resource
`extmem-provider`, for example `--inherit-fd fd[4]:extmem-provider`.
The descriptor is moved through CRIU's fdstore before restore starts.

CRIU sends one protobuf `extmem_req` packet per request and receives one
protobuf `extmem_resp` packet. A successful `OPEN_IMAGE`, `GET_VMA`, or
`GET_SHARED` response carries exactly one descriptor with `SCM_RIGHTS`.
`images/extmem.proto` uses proto2.
Each request and response must fit in an 8 KiB packet. CRIU rejects a response
with any unexpected ancillary data.
`OPEN_IMAGE` names only a relative CRIU image path and its open flags; the
provider never receives an image-directory or checkpoint-root descriptor.

The operation sequence is `INIT`, zero or more image and memory requests,
`WAIT_READY`, then `COMMIT` on successful restore or `ABORT` on failure.
After accepting `INIT`, a provider must support `WAIT_READY`; `-ENOTSUP` for
that operation is a protocol error. `WAIT_READY` completes before CRIU runs
pre-resume scripts. Private mappings are identified by
`(pid, vma_id, vaddr, length)` and shared objects by
`(shmid, length)`. A successful memory request returns a provider-backed
memfd for one complete object; CRIU does not copy page-image contents
into it. Each restoring task requests and maps eligible private anonymous VMAs
before it enters PIE; PIE then moves the existing mappings to their final
addresses. Hugetlb and other special mappings retain their native restore
paths.

For an unsupported image the provider returns `-ENOTSUP`; CRIU then uses its
local restore path for that image. After `INIT` succeeds, CRIU marks every
eligible private anonymous VMA as provider-backed and requires the provider to
return every shared object. `GET_VMA` and `GET_SHARED` must succeed;
`-ENOTSUP` or any other error aborts the restore. Forced-local images are
always opened by CRIU and are not offered to the provider. Returned memory
descriptors must be memfds at least as large as the requested object; CRIU
checkpoints provider-backed mappings through those memfds. A
descriptor for a checkpointed memfd must also support seals. Its existing seals
must be a subset of the saved seals and must not include `F_SEAL_SEAL`, because
CRIU reapplies the saved seals after `WAIT_READY`.
For a huge-page mapping, the descriptor must use the matching huge-page size.
