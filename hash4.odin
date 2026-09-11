//WAL chunk verification: hashing multi-kilobyte log segments that arrive in streams
//Large BLOB/Text columns: hashing 100MB files piece-by-piece so you dont load the entire
//file into RAM.
package main

import "core:fmt"
import "core:hash/xxhash"

main :: proc() {
    // XXH64 streaming state (No 64-byte alignment requirement like XXH3)
    state: xxhash.XXH64_state
    seed: u64 = 0
    xxhash.XXH64_reset_state(&state, seed)

    chunk1 := transmute([]u8)"WAL block 1..."
    chunk2 := transmute([]u8)"WAL block 2..."

    xxhash.XXH64_update(&state, chunk1)
    xxhash.XXH64_update(&state, chunk2)

    digest := xxhash.XXH64_digest(&state)
    fmt.printfln("Streaming XXH64 Digest: 0x%016x", digest)
}
