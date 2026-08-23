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
The provider is restore-only in this version. CRIU does not offer dump-time
image opens to it; dump continues to use CRIU's normal image path.

The operation sequence is `INIT`, zero or more image and memory requests,
`WAIT_READY`, then `COMMIT` on successful restore or `ABORT` on failure.
After accepting `INIT`, a provider must support `WAIT_READY`; `-ENOTSUP` for
that operation is a protocol error. A successful `WAIT_READY` response tells
CRIU that the provider has finished populating the returned objects. CRIU
then applies the saved memfd seals, and runs `ACT_PRE_RESUME` only after that.
The order is `WAIT_READY` -> `apply_memfd_seals()` -> `ACT_PRE_RESUME` ->
task resume. Private mappings are identified by
`(pid, vma_id, vaddr, length)` and shared objects by
`(shmid, length)`. A successful memory request returns a file descriptor for
one complete object; CRIU maps it directly and does not copy page-image
contents into it. Each restoring task requests and maps eligible private
anonymous VMAs before it enters PIE; PIE then moves the existing mappings to
their final addresses. For provider-backed private hugetlb VMAs, CRIU obtains
the provider FD in `open_vmas()` and maps it at the final address instead of
premapping it early.
`vma_id` is the restore-side VMA ordinal used
with the other fields to identify a private mapping; it is not a durable
checkpoint ID.

For an unsupported image the provider returns `-ENOTSUP`; CRIU then uses its
local restore path for that image. After `INIT` succeeds, CRIU marks every
eligible private anonymous VMA as provider-backed and requests each one
individually. If `GET_VMA` returns `-ENOTSUP`, CRIU clears the provider-backed
state for that VMA and uses its normal anonymous-memory restore path. If
`GET_SHARED` returns `-ENOTSUP`, CRIU uses its normal shmem or memfd restore
path. Other provider errors abort the restore. Forced-local images are always
opened by CRIU and are not offered to the provider.

For provider-backed anonymous VMAs and shmem mappings, CRIU maps the returned
file descriptor directly and does not require a particular file type or
validate its backing size first. The provider must return an FD that can back
the requested mapping. A provider-supplied descriptor for a checkpointed memfd
is validated separately: CRIU requires a regular file at least as large as the
requested object and checks its seals. Its existing seals must be a subset of
the saved seals and must not include `F_SEAL_SEAL`, because CRIU reapplies the
saved seals after `WAIT_READY`.
For a huge-page mapping, the descriptor must use the matching huge-page size.

For `OPEN_IMAGE`, CRIU passes the returned descriptor through its existing
buffered image reader. A mmap-based image reader can be considered separately,
but non-page images are small compared with the memory payload, so the
expected gain is limited.
