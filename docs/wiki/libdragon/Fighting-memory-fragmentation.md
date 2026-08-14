# Fighting memory fragmentation

The Nintendo 64 has **4 MiB of RDRAM** without an Expansion Pak and **8 MiB** with one. That memory is shared by everything: executable code, global data, stacks, framebuffers, audio buffers, graphics data, game state, loaded assets, dynamic libraries, and the heap. On a 4 MiB console, the part left for dynamic allocation can become small surprisingly quickly.

This guide explains how heap fragmentation happens, what libdragon's regular allocator already does for you, and how to use libdragon's scratch allocator to keep temporary work from damaging the long-lived heap.

> This page describes the libdragon `preview` branch and its scratch allocator

## 1. Fragmentation and memory pressure

A call to `malloc(320 * 1024)` needs **one contiguous 320 KiB region**. It is not enough for the heap to contain 320 KiB in total if that space is split into smaller holes.

<img width="1360" height="590" alt="fragmented-oom" src="https://github.com/user-attachments/assets/71075fa8-9472-4c1b-8f4c-671ee7f9625d" />

In the example above, the heap contains 440 KiB free, but its largest free chunk is only 180 KiB. The 320 KiB allocation therefore fails.

This is **external fragmentation**: free memory exists, but live allocations separate it into pieces. A general-purpose C allocator cannot fix this by moving live objects because all pointers to those objects would become invalid.

There is also **internal fragmentation**: an allocation consumes more memory than requested because of alignment, allocator metadata, or an oversized recycled block. For example, on a typical libdragon configuration, even a very small request occupies a minimum-sized chunk of 16 bytes, and every chunk carries hidden bookkeeping information.

### Memory pressure is more than "almost all RAM is used"

A game is under memory pressure when one or more of these conditions is true:

- little total memory remains;
- the largest contiguous free chunk is small;
- a loading operation temporarily needs both old and new data at once;
- `realloc()` may need a new buffer before releasing the old one;
- temporary workspaces are mixed among long-lived objects;
- framebuffers, audio, decompression, or rendering create large short-lived peaks.

On a 4 MiB N64, a temporary 200 KiB allocation is not a minor detail. It may be a significant fraction of the entire remaining heap. A memory layout that is harmless on an 8 MiB development console can fail on a base console after several level transitions.

> [!IMPORTANT]
> **Memory availability is determined by total free memory, the largest free chunk, and the peak live memory required by the current operation.**

## 2. A quick tour of dlmalloc

libdragon's normal `malloc()`, `calloc()`, `realloc()`, and `free()` use a general-purpose allocator derived from **Doug Lea's malloc**, usually called **dlmalloc**. You do not need to study allocator internals to use it well. These five ideas are enough:

- **Chunks and metadata.** Each allocation occupies a chunk slightly larger than the requested payload. The extra space stores bookkeeping and satisfies alignment. Very small requests therefore still consume a minimum-sized chunk.

- **Size-class bins, including fastbins.** Free chunks are grouped by size so the allocator can quickly find a suitable one. Recently freed small chunks are kept in especially fast lists for likely same-size reuse.

- **Best-fit and splitting.** For larger requests, dlmalloc tries to use the smallest available free chunk that is large enough. If that chunk is oversized, it can split off the requested part and keep the remainder free.

- **Coalescing and the top chunk.** Adjacent normal free chunks are merged into larger chunks. The unused space at the growing end of the heap is kept as one special free region called the top chunk.

- **`realloc()` can move, but the heap cannot compact itself.** A block can grow in place only when suitable space is immediately after it. Otherwise dlmalloc allocates a new block, copies the data, and frees the old one. It cannot move arbitrary live objects to join holes because application pointers would become invalid.

These features already handle equal-size reuse, splitting, neighboring frees, and sensible hole selection. 

> [!IMPORTANT]
> Application code should focus on the information dlmalloc does not have: **object lifetime, which resources can be released early, and the peak memory required by an operation.**

## 3. The libdragon scratch allocator

libdragon's scratch allocator provides a second allocation API for temporary data:

```c
#include <scratch.h>

void *scratch_malloc(size_t size);
void *scratch_calloc(size_t count, size_t size);
void *scratch_realloc(void *ptr, size_t size);
void *scratch_memalign(size_t alignment, size_t size);
void *scratch_malloc_uncached(size_t size);
void  scratch_free(void *ptr);
```

It does **not** add more RAM. The regular heap and scratch allocator reserve memory from opposite ends of the same available heap space.

<img width="1469" height="362" alt="two-ended-heap" src="https://github.com/user-attachments/assets/5c1c6927-5ebf-417b-8711-7b9163677c2b" />

The regular allocator grows from low addresses through `sbrk()`. Scratch reserves from the high-address side through `sbrk_top()`. Keeping temporary work at the other end prevents it from being physically interleaved with long-lived regular allocations.

The scratch allocator is intentionally simple:

- first-fit reuse of existing free scratch blocks;
- 16-byte-aligned returned pointers;
- a doubly linked physical block list;
- arbitrary `scratch_free()` calls are accepted;
- blocks are **not split** when an oversized free block is reused;
- neighboring free blocks are **not coalesced**;
- memory is returned to the shared heap only while free blocks are at the scratch-list head.

This last point makes short, nested, near-LIFO lifetimes especially effective:

<img width="1969" height="498" alt="scratch-release-order" src="https://github.com/user-attachments/assets/b59c33bc-480d-4d00-b849-31b935ea00e2" />

Scratch is ideal for:

- file-loading buffers;
- decompression workspaces;
- temporary image or audio conversion data;
- staging buffers used during DMA;
- temporary relocation and symbol data while loading a DSO;
- short-lived intermediate arrays used to build a persistent result.

Scratch is not a second general-purpose heap. Long-lived scratch allocations pin the scratch reservation, and repeated reuse of oversized blocks can waste memory because blocks are never split.

---

# 4. Core rules

The following four DOs and three DON'Ts cover most fragmentation problems seen in N64 games. They focus on decisions the application can control: lifetime, temporary work, peak memory, and whether a specialized allocation scheme is actually justified.

## DO 1: Separate allocations by lifetime

> [!IMPORTANT]
> **Keep objects that die together close together, and keep short-lived work away from long-lived state.**

The most damaging heap pattern is not simply “many allocations.” It is an alternating sequence of allocations with very different lifetimes:

```text
persistent, temporary, persistent, temporary, persistent, temporary
```

After the temporary objects are released, the persistent objects remain as small barriers dividing the heap into holes.

<img width="2427" height="481" alt="lifetime-separation" src="https://github.com/user-attachments/assets/e1163207-83c9-406c-8fed-1345e27573e0" />

### Why this works

Dlmalloc can merge adjacent free chunks, but it cannot merge across a live allocation. If temporary objects are grouped together, freeing them produces adjacent free chunks that can become one large region. If they are interleaved with long-lived objects, no allocator policy can join the holes without moving the live objects and invalidating their pointers.

Apply this rule at subsystem boundaries:

- allocate permanent engine state during startup;
- create level-owned state in a clearly defined loading phase;
- keep decompression, parsing, conversion, and relocation work in scratch;
- release file handles, decoder state, and staging data as soon as the persistent result is complete;
- avoid creating unrelated long-lived allocations in the middle of a temporary loading sequence.

When a known set of objects has exactly the same lifetime, a **single packed allocation** can be useful. For example, one block may contain a level header, a fixed entity array, lookup tables, and strings at aligned offsets. This reduces allocator metadata and guarantees that unrelated allocations cannot be inserted between the parts.

Do not force this design when the layout is naturally dynamic or when libdragon APIs allocate their results internally. A series of ordinary `malloc()` calls made in the same loading phase is still reasonable. The important point is to avoid mixing those long-lived results with temporary regular-heap workspaces.

This rule matters more than allocating the largest object first, freeing in address order, or trying to predict which dlmalloc bin will be used. **Lifetime is application knowledge that dlmalloc does not have.**

## DO 2: Use scratch for short-lived workspaces

> [!IMPORTANT]
> **Put data in scratch when it exists only to produce, load, or transform something else.**

A typical loader should allocate its final result from the regular heap and its workspace from scratch:

```c
asset_t *load_asset(const char *path)
{
    size_t result_size = asset_result_size(path);
    size_t work_size   = asset_workspace_size(path);

    asset_t *asset = malloc(result_size);
    if (!asset)
        return NULL;

    void *work = scratch_malloc(work_size);
    if (!work) {
        free(asset);
        return NULL;
    }

    bool ok = decode_asset(path, asset, work);
    scratch_free(work);

    if (!ok) {
        free(asset);
        return NULL;
    }

    return asset;
}
```

When several scratch buffers are nested, release them in reverse order when practical:

```c
void *input = scratch_malloc(input_size);
void *coeff = scratch_malloc(coeff_size);
void *image = scratch_malloc(image_size);

/* Build the persistent result. */

scratch_free(image);
scratch_free(coeff);
scratch_free(input);
```

### Why this works

The regular heap grows from low addresses while scratch reserves memory from the high-address side. Temporary scratch blocks therefore cannot become holes between ordinary long-lived allocations.

Reverse-order release is especially effective because the scratch allocator can return free blocks to `sbrk_top()` only while those blocks are at the head of its physical list. Arbitrary `scratch_free()` calls are valid, but an older free block behind newer live blocks remains reserved until the newer blocks disappear.

Scratch works best for:

- decompression and parsing workspaces;
- temporary file contents;
- image, audio, mesh, or font conversion buffers;
- short-lived staging buffers;
- intermediate arrays used to build a persistent result.

Useful phase-boundary checks include:

```c
scratch_stats_t stats;
scratch_get_stats(&stats);

assert(scratch_empty());  /* for example, after level loading */
```

A large difference between scratch `live_bytes` and `reserved_bytes` is a sign that blocks were freed out of order, an oversized free block was reused for a small request, or a scratch allocation escaped its intended scope.

## DO 3: Allocate persistent results at their final size and control buffer growth

> [!IMPORTANT]
> **Avoid making long-lived objects carry temporary capacity, and treat every possible `realloc()` move as a peak-memory event.**

If a loader needs `P` bytes that remain alive and `T` bytes only while decoding, the robust default is:

```c
void *persistent = malloc(P);
void *temporary  = scratch_malloc(T);
```

rather than:

```c
void *combined = malloc(P + T);
/* use the tail as temporary storage */
combined = realloc(combined, P);
```

The combined version requires one contiguous `P + T` chunk in the regular heap. The separate version requires only `P` there; `T` is reserved from the scratch side. Under memory pressure, the separate design can succeed even when no contiguous `P + T` region exists.

Shrinking a combined block is not inherently wrong. Dlmalloc can normally split off the unused tail, and the pattern can be efficient inside a tightly controlled leaf function where no other allocations occur before the shrink. Treat it as a measured local optimization, not as the default loading architecture. If another live allocation appears after the combined block, the released tail becomes a reusable hole rather than returning to the top chunk.

When the final size is unknown, grow capacity geometrically rather than by a few bytes at a time:

```c
size_t new_capacity = capacity ? capacity + capacity / 2 : 256;
while (new_capacity < required)
    new_capacity += new_capacity / 2;

void *new_ptr = realloc(ptr, new_capacity);
if (!new_ptr) {
    /* ptr is still valid. */
    return false;
}
ptr = new_ptr;
capacity = new_capacity;
```

### Why this works

Dlmalloc first tries to grow a chunk in place by consuming an adjacent free chunk or the top chunk. If that is impossible, `realloc()` must allocate the new buffer, copy the old contents, and only then free the old buffer. During that operation both buffers coexist.

For example, growing a 300 KiB buffer to 450 KiB may temporarily require roughly 750 KiB plus allocator overhead. The operation can fail even if a 450 KiB final buffer would fit after the old one disappeared.

Prefer these strategies, in order:

1. store enough metadata in the asset format to know final and workspace sizes;
2. use a two-pass builder: count first, allocate once, fill second;
3. reserve a reasonable known capacity once;
4. grow geometrically when exact sizing is impossible;
5. build temporarily in scratch, then copy once into an exact persistent allocation.

Always assign `realloc()` through a temporary pointer. On failure, the original allocation remains valid.

## DO 4: Reduce peak memory before trying to optimize fragmentation

> [!IMPORTANT]
> **Release resources that are no longer needed before allocating their replacements whenever the transition allows it.**

Fragmentation is only one cause of allocation failure. A loader often fails because the old and new states coexist, together with the loading workspace:

```text
old level + new level + decompression workspace + transition effects
```

<img width="978" height="721" alt="peak-allocation" src="https://github.com/user-attachments/assets/ebc051de-5cf6-4afc-8a77-fd0c76934c3d" />

The most effective optimization may therefore be changing the order of operations:

```text
stop using old resource
free old resource
load new resource
publish new resource
```

instead of:

```text
load new resource
publish new resource
free old resource
```

### Why this works

No allocator can compensate for a peak that exceeds available RAM. If an old 700 KiB level and a new 800 KiB level coexist with a 250 KiB workspace, the transition needs 1.75 MiB even before other engine memory is counted. Freeing the old level first removes 700 KiB from the peak and usually creates useful contiguous space at the same time.

Look for opportunities to:

- release the previous level's textures, meshes, scripts, dynamic libraries, and audio before loading replacements;
- stop music and sound effects and release their buffers before opening the next music or sound effect bank;
- destroy temporary menus, previews, or loading-screen assets before the largest load begins;
- serialize independent loading jobs instead of running all their workspaces simultaneously;
- reuse one scratch workspace for consecutive stages rather than allocating all stage buffers together;
- stream or process data in chunks when the full input and full output do not both need to be resident;
- overwrite or transform data in place when the format and algorithm make that safe;
- keep only a small transition state while neither full level is active.

Sometimes old and new resources must coexist: seamless streaming, cross-fades, rollback, or failure-safe hot replacement may require it. In that case, treat the overlap as an explicit budget, not an accidental implementation detail. Possible compromises include loading a reduced transition set, splitting the new resource into independently publishable pieces, or retaining only the old subset needed until the handover completes.

This rule also applies inside one loader. Free an input buffer as soon as its last consumer finishes; do not keep every intermediate representation alive until the function returns. The lifetime of a local pointer is not automatically the lifetime of the whole operation.

## DON'T 1: Do not treat scratch as a second general-purpose heap

> [!IMPORTANT]
> **Do not keep level state, persistent caches, open-ended containers, or arbitrary-lifetime objects in scratch.**

Scratch accepts out-of-order frees, but it deliberately lacks several features of dlmalloc:

- oversized free blocks are reused without splitting;
- neighboring free blocks are not coalesced;
- only free blocks at the physical head are returned to the shared heap.

### Why not

If a 256 KiB free scratch block satisfies a later 20 KiB request, the entire block remains reserved for that request. Two adjacent free 64 KiB scratch blocks do not become a 128 KiB block. A long-lived scratch block can also pin older free blocks behind it, reducing the address-space gap available to the regular heap.

These trade-offs are intentional: scratch is optimized for simple, temporary, near-LIFO work. When an object survives into gameplay, has uncertain lifetime, or participates in arbitrary churn, use the regular heap or a deliberately packed owner allocation.

## DON'T 2: Do not introduce an arena by default

> [!IMPORTANT]
> **An arena is not automatically better than dlmalloc just because several objects belong to one level or phase.**

An arena typically reserves one large block and serves cheap bump allocations from it. Individual objects are not returned to the main heap; memory becomes reusable outside the arena only when the whole arena is reset or destroyed.

This sounds ideal for same-lifetime objects, but on libdragon it is often unnecessary or counterproductive:

- the arena itself needs one large contiguous allocation up front;
- its capacity is usually based on a worst-case estimate, so unused capacity remains unavailable to the rest of the game;
- objects freed early do not reduce memory pressure before the reset point;
- ordinary heap holes elsewhere cannot satisfy allocations inside the arena;
- many libdragon APIs, such as high-level asset loaders, allocate internally and therefore bypass an application arena unless an `_into`-style API exists or the library is changed;
- loading a new arena before destroying the old one creates a particularly large generation-to-generation peak;
- ownership, destructors, and error cleanup become more complicated when some objects escape the intended reset boundary.

Dlmalloc already handles variable-sized allocations, individual frees, hole reuse, splitting, and coalescing. If objects have independent lifetimes or may be released before the end of the level, the normal heap usually makes better use of scarce RAM.

For a known collection with exactly one lifetime, a single explicitly laid-out allocation is often simpler than implementing a general arena. For temporary loader work, scratch already provides lifetime separation without permanently reserving a guessed level-sized block.

### When an arena can still be justified

Consider an arena only when most of these statements are true:

- every allocation stored in it is controlled by your code;
- all objects have a strict, common reset point;
- individual frees provide no useful reduction in peak memory;
- there are many small or variable-sized objects whose final layout is not known in advance;
- the required capacity can be budgeted accurately;
- the old arena can be destroyed before a replacement arena is allocated;
- deterministic bump-allocation cost is materially useful.

Possible examples include a fixed-capacity per-frame command builder, a parser that creates many temporary nodes and discards all of them at once, or a completely self-contained level representation with a measured maximum size. Even in these cases, compare against a fixed buffer, one packed allocation, scratch, or ordinary heap allocations before adding another allocator.

## DON'T 3: Do not fight dlmalloc with allocator folklore

> [!IMPORTANT]
> **Do not add complexity for problems that dlmalloc already solves.**

The following techniques are normally ineffective or actively wasteful:

- freeing adjacent chunks in address order;
- sorting all `free()` calls;
- allocating every object largest-first;
- rounding every request to the next power of two;
- requesting excessive alignment "just in case";
- calling `malloc_trim()` as if it compacted the heap;
- allocating and freeing one huge block to "prepare" the heap;
- adding a generic caching layer in front of every small allocation;
- disabling fastbins solely because deferred coalescing looks dangerous.

### Why not

Dlmalloc already groups free chunks by size, quickly reuses small equal-sized chunks, splits oversized free chunks, coalesces adjacent normal free chunks, and applies best-fit behavior to larger requests. It also consolidates deferred small chunks when larger contiguous memory is needed.

None of these mechanisms can cross a live allocation, so application effort should go into lifetime separation and peak reduction instead. Blanket power-of-two rounding increases internal fragmentation. Excessive `memalign()` can create leading and trailing fragments. `malloc_trim()` may release free memory at the top, but it cannot relocate live objects or join holes separated by them.

A specialized allocation scheme is worthwhile only when it expresses information dlmalloc cannot know, such as a guaranteed shared reset point or a strict deterministic-time requirement. It is not useful merely as another layer over the same arbitrary allocation pattern.


## 5. Allocation patterns that dlmalloc already handles well

Some common patterns need no application-level workaround:

- Repeatedly allocating and freeing the same small size is efficient because small chunks are cached for reuse.
- Freeing adjacent normal chunks in either order leads to the same coalesced result.
- A freed chunk larger than a request can be split and reused.
- Large requests preferentially consume a close-fitting hole instead of wasting the largest available region.
- Fastbin chunks are consolidated when larger contiguous memory is required.

Do not spend engineering effort micromanaging these cases. Concentrate on lifetime domains, large contiguous requests, and transient coexistence of old and new buffers.

## 6. Practical checklist for a level loader

A robust loader for a 4 MiB target should follow a sequence similar to this:

1. Determine persistent sizes and workspace sizes from file metadata where possible.
2. Identify which old resources can be released before loading their replacements.
3. Allocate final persistent objects at their final size from the regular heap.
4. Allocate decompression, parsing, relocation, and conversion workspaces from scratch.
5. Reuse or free each intermediate buffer immediately after its last use.
6. Serialize large loading stages when simultaneous workspaces are not required.
7. Free scratch workspaces promptly, preferably in reverse order.
8. Close files and destroy loader state as soon as they are no longer needed.
9. Verify that scratch is empty and record the peak regular and scratch usage.
10. Repeat load/unload cycles on a 4 MiB configuration; do not test only the first load or only with an Expansion Pak.

## 7. References

- [Doug Lea malloc source used for the allocator discussion](https://github.com/ennorehling/dlmalloc/blob/master/malloc.c)
- [libdragon scratch allocator implementation, `preview` branch](https://github.com/DragonMinded/libdragon/blob/preview/src/scratch.c)
- [libdragon scratch allocator public API, `preview` branch](https://github.com/DragonMinded/libdragon/blob/preview/include/scratch.h)
- [Nintendo 64 RDRAM overview](https://n64brew.dev/wiki/RDRAM)
