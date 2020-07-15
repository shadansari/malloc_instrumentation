# malloc_instrumentation

run a binary with formatted malloc/free calls

with this collection of tools, you will be able to run any binary and
augment its output and periods of inactivity with memory usage
differentials

## Compilation
    'make' generates malloc_instrument.so library.

## Usage
    LD_PRELOAD=<path-to>/malloc_instrument.so <executable>
    e.g. LD_PRELOAD=./malloc_instrument.so python

## TODO
- No support yet for reporting 'Current allocations by age'.

## Testing
- 'make test' runs a basic test case (no threading).
- 'LD_PRELOAD=./malloc_instrument.so python'
- 'LD_PRELOAD=./malloc_instrument.so firefox'
- Used a multi-thread test for malloc performance from ptmalloc to stress test multi-threading (https://github.com/emeryberger/Malloc-Implementations/blob/master/allocators/ptmalloc/ptmalloc3/t-test1.c)

## References
- https://stackoverflow.com/questions/6083337/overriding-malloc-using-the-ld-preload-mechanism/10008252#10008252
- https://github.com/jtolio/malloc_instrumentation
- https://github.com/emeryberger/Malloc-Implementations/blob/master/allocators/ptmalloc/ptmalloc3/t-test1.c
- https://github.com/troydhanson/uthash

## ISSUES
- Fix seg fault with 'LD_PRELOAD=./malloc_instrument.so find /'